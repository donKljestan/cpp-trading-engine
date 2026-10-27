#pragma once

// Single-pass JSON scanners.
//
// Not a JSON parser: these walk a value and hand back raw spans, leaving
// interpretation to the caller. Exchange payloads have a fixed shape, so this is both
// faster than a general parser and free of allocation.
//
// Repeated find("\"key\":") per field would rescan the buffer once per field; these
// visit every byte once.

#include <cstddef>
#include <string_view>

#include "llt/core/fixed.hpp"

namespace llt::json {

[[nodiscard]] constexpr std::string_view unquote(std::string_view v) noexcept {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

[[nodiscard]] constexpr bool parse_uint(std::string_view s, uint64_t& out) noexcept {
    s = unquote(s);
    if (s.empty()) return false;

    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

[[nodiscard]] constexpr bool parse_bool(std::string_view s, bool& out) noexcept {
    s = unquote(s);
    if (s == "true") {
        out = true;
        return true;
    }
    if (s == "false") {
        out = false;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool parse_decimal(std::string_view s, Fixed& out) noexcept {
    s = unquote(s);
    return parse_fixed(s.data(), s.data() + s.size(), out);
}

namespace detail {

class Cursor {
public:
    constexpr explicit Cursor(std::string_view s) noexcept : s_(s) {}

    [[nodiscard]] constexpr bool   at_end() const noexcept { return i_ >= s_.size(); }
    [[nodiscard]] constexpr char   peek() const noexcept { return s_[i_]; }
    [[nodiscard]] constexpr size_t pos() const noexcept { return i_; }

    constexpr void advance() noexcept { ++i_; }

    [[nodiscard]] constexpr std::string_view slice_from(size_t begin) const noexcept {
        return s_.substr(begin, i_ - begin);
    }

    constexpr void skip_ws() noexcept {
        while (!at_end() &&
               (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            advance();
        }
    }

    constexpr bool skip_string() noexcept {
        advance();  // opening quote
        while (!at_end() && peek() != '"') {
            if (peek() == '\\') advance();
            advance();
        }
        if (at_end()) return false;
        advance();
        return true;
    }

    // Steps over one complete JSON value, whatever its type.
    constexpr bool skip_value() noexcept {
        if (at_end()) return false;

        const char open = peek();
        if (open == '"') return skip_string();

        if (open == '{' || open == '[') {
            const char close = (open == '{') ? '}' : ']';
            int        depth = 0;
            while (!at_end()) {
                const char c = peek();
                if (c == '"') {
                    if (!skip_string()) return false;
                    continue;
                }
                if (c == open) {
                    ++depth;
                } else if (c == close) {
                    --depth;
                    if (depth == 0) {
                        advance();
                        return true;
                    }
                }
                advance();
            }
            return false;
        }

        while (!at_end() && peek() != ',' && peek() != '}' && peek() != ']') advance();
        return true;
    }

private:
    std::string_view s_;
    size_t           i_ = 0;
};

}  // namespace detail

// Walks the key/value pairs of one object. Strings keep their quotes; nested objects
// and arrays come back whole, braces included.
class ObjectScanner {
public:
    explicit constexpr ObjectScanner(std::string_view obj) noexcept : c_(obj) {
        c_.skip_ws();
        if (!c_.at_end() && c_.peek() == '{') c_.advance();
    }

    [[nodiscard]] constexpr bool next(std::string_view& key,
                                      std::string_view& value) noexcept {
        c_.skip_ws();
        if (!c_.at_end() && c_.peek() == ',') {
            c_.advance();
            c_.skip_ws();
        }
        if (c_.at_end() || c_.peek() == '}') return false;
        if (c_.peek() != '"') return false;

        c_.advance();
        const size_t key_begin = c_.pos();
        while (!c_.at_end() && c_.peek() != '"') c_.advance();
        if (c_.at_end()) return false;
        key = c_.slice_from(key_begin);
        c_.advance();

        c_.skip_ws();
        if (c_.at_end() || c_.peek() != ':') return false;
        c_.advance();
        c_.skip_ws();

        const size_t value_begin = c_.pos();
        if (!c_.skip_value()) return false;
        value = c_.slice_from(value_begin);
        return true;
    }

private:
    detail::Cursor c_;
};

// Walks the elements of an array, returning each one raw.
class ArrayScanner {
public:
    explicit constexpr ArrayScanner(std::string_view arr) noexcept : c_(arr) {
        c_.skip_ws();
        if (!c_.at_end() && c_.peek() == '[') c_.advance();
    }

    [[nodiscard]] constexpr bool next(std::string_view& element) noexcept {
        c_.skip_ws();
        if (!c_.at_end() && c_.peek() == ',') {
            c_.advance();
            c_.skip_ws();
        }
        if (c_.at_end() || c_.peek() == ']') return false;

        const size_t begin = c_.pos();
        if (!c_.skip_value()) return false;
        element = c_.slice_from(begin);
        return true;
    }

private:
    detail::Cursor c_;
};

// Walks an array of two-element arrays: [["80944.60","0.742"],...].
class PairArrayScanner {
public:
    explicit constexpr PairArrayScanner(std::string_view arr) noexcept : outer_(arr) {}

    [[nodiscard]] constexpr bool next(Fixed& first, Fixed& second) noexcept {
        std::string_view element;
        if (!outer_.next(element)) return false;

        ArrayScanner     parts(element);
        std::string_view a;
        std::string_view b;
        if (!parts.next(a) || !parts.next(b)) return false;
        return parse_decimal(a, first) && parse_decimal(b, second);
    }

private:
    ArrayScanner outer_;
};

// Finds one top-level field without interpreting the rest of the object.
[[nodiscard]] constexpr bool find_field(std::string_view obj, std::string_view key,
                                        std::string_view& out) noexcept {
    ObjectScanner    sc(obj);
    std::string_view k;
    std::string_view v;
    while (sc.next(k, v)) {
        if (k == key) {
            out = v;
            return true;
        }
    }
    return false;
}

}  // namespace llt::json
