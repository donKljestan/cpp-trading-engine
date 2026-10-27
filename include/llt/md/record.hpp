#pragma once

// Market data capture format.
//
// Raw websocket payloads are stored, not decoded events. A recording then stays valid
// when the parser changes, and doubles as a regression corpus for the parser itself.
// Decoding on replay goes through the same dispatch as the live feed, so a backtest
// exercises the real program rather than a parallel implementation of it.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace llt::md {

inline constexpr uint32_t kMagic   = 0x5254'4C4CU;  // "LLTR"
inline constexpr uint16_t kVersion = 1;

enum class RecordKind : uint8_t {
    WsText = 0,
};

struct FileHeader {
    uint32_t magic         = kMagic;
    uint16_t version       = kVersion;
    uint16_t flags         = 0;
    uint64_t start_wall_ns = 0;
    uint64_t start_mono_ns = 0;
    uint64_t tsc_hz        = 0;  // so recorded TSC deltas can be read back as time
    uint64_t reserved      = 0;
};
static_assert(sizeof(FileHeader) == 40, "file header must have no padding");

struct RecordHeader {
    uint64_t recv_tsc     = 0;
    uint64_t recv_wall_ns = 0;
    uint32_t length       = 0;
    uint8_t  kind         = 0;
    uint8_t  pad[3]       = {};
};
static_assert(sizeof(RecordHeader) == 24, "record header must have no padding");

class RecordWriter {
public:
    RecordWriter() = default;
    ~RecordWriter();

    RecordWriter(const RecordWriter&)            = delete;
    RecordWriter& operator=(const RecordWriter&) = delete;

    [[nodiscard]] bool open(const std::string& path);
    void               close() noexcept;

    [[nodiscard]] bool write(uint64_t recv_tsc, uint64_t recv_wall_ns,
                             std::string_view payload);

    [[nodiscard]] bool     is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] uint64_t records() const noexcept { return records_; }
    [[nodiscard]] uint64_t bytes() const noexcept { return bytes_; }

private:
    std::FILE*        file_ = nullptr;
    std::vector<char> buffer_;
    uint64_t          records_ = 0;
    uint64_t          bytes_   = 0;
};

class RecordReader {
public:
    RecordReader() = default;
    ~RecordReader();

    RecordReader(const RecordReader&)            = delete;
    RecordReader& operator=(const RecordReader&) = delete;

    [[nodiscard]] bool open(const std::string& path);
    void               close() noexcept;

    [[nodiscard]] const FileHeader& header() const noexcept { return header_; }

    // Returns false at clean end of file. The payload points into an internal buffer
    // and is invalidated by the next call.
    [[nodiscard]] bool next(RecordHeader& out, std::string_view& payload);

    // True when reading stopped on a malformed or truncated record rather than at EOF.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    std::FILE*        file_ = nullptr;
    FileHeader        header_{};
    std::vector<char> payload_;
    bool              truncated_ = false;
};

}  // namespace llt::md
