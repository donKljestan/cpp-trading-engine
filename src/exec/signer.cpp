#include "llt/exec/signer.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace llt::exec {
namespace {

thread_local char t_last_error[256] = {};

void record(const char* what) noexcept {
    const unsigned long code = ::ERR_get_error();
    if (code != 0) {
        char detail[192];
        ::ERR_error_string_n(code, detail, sizeof(detail));
        std::snprintf(t_last_error, sizeof(t_last_error), "%s: %s", what, detail);
    } else {
        std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
    }
}

bool base64_encode(const uint8_t* data, size_t len, std::string& out) {
    out.resize(4 * ((len + 2) / 3) + 1);
    const int n = ::EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data,
                                    static_cast<int>(len));
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    return true;
}

bool base64_decode(std::string_view in, std::vector<uint8_t>& out) {
    out.resize(3 * (in.size() / 4) + 4);
    const int n = ::EVP_DecodeBlock(out.data(),
                                    reinterpret_cast<const unsigned char*>(in.data()),
                                    static_cast<int>(in.size()));
    if (n < 0) return false;

    // EVP_DecodeBlock reports the padded length, so trim what the padding stood for.
    size_t len = static_cast<size_t>(n);
    for (auto it = in.rbegin(); it != in.rend() && *it == '='; ++it) --len;
    out.resize(len);
    return true;
}

}  // namespace

Ed25519Signer::~Ed25519Signer() { reset(); }

Ed25519Signer::Ed25519Signer(Ed25519Signer&& other) noexcept
    : key_(std::exchange(other.key_, nullptr)) {}

Ed25519Signer& Ed25519Signer::operator=(Ed25519Signer&& other) noexcept {
    if (this != &other) {
        reset();
        key_ = std::exchange(other.key_, nullptr);
    }
    return *this;
}

void Ed25519Signer::reset() noexcept {
    if (key_ != nullptr) {
        ::EVP_PKEY_free(key_);
        key_ = nullptr;
    }
}

bool Ed25519Signer::generate() {
    reset();
    key_ = ::EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    if (key_ == nullptr) {
        record("EVP_PKEY_Q_keygen");
        return false;
    }
    return true;
}

bool Ed25519Signer::load_seed(const uint8_t seed[32]) {
    reset();
    key_ = ::EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
    if (key_ == nullptr) {
        record("EVP_PKEY_new_raw_private_key");
        return false;
    }
    return true;
}

bool Ed25519Signer::load_pem(std::string_view pem) {
    reset();

    BIO* bio = ::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        record("BIO_new_mem_buf");
        return false;
    }

    key_ = ::PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    ::BIO_free(bio);

    if (key_ == nullptr) {
        record("PEM_read_bio_PrivateKey");
        return false;
    }
    if (::EVP_PKEY_get_base_id(key_) != EVP_PKEY_ED25519) {
        record("key is not Ed25519");
        reset();
        return false;
    }
    return true;
}

bool Ed25519Signer::load_pem_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::snprintf(t_last_error, sizeof(t_last_error), "cannot open %s", path.c_str());
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return load_pem(buf.str());
}

bool Ed25519Signer::sign(std::string_view message, std::string& out_b64) const {
    if (key_ == nullptr) {
        record("no key loaded");
        return false;
    }

    EVP_MD_CTX* ctx = ::EVP_MD_CTX_new();
    if (ctx == nullptr) {
        record("EVP_MD_CTX_new");
        return false;
    }

    bool ok = false;
    // Ed25519 signs in one shot; there is no streaming update step.
    if (::EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key_) == 1) {
        size_t len = 0;
        if (::EVP_DigestSign(ctx, nullptr, &len,
                             reinterpret_cast<const unsigned char*>(message.data()),
                             message.size()) == 1) {
            std::vector<uint8_t> sig(len);
            if (::EVP_DigestSign(ctx, sig.data(), &len,
                                 reinterpret_cast<const unsigned char*>(message.data()),
                                 message.size()) == 1) {
                sig.resize(len);
                ok = base64_encode(sig.data(), sig.size(), out_b64);
            }
        }
    }
    if (!ok) record("EVP_DigestSign");

    ::EVP_MD_CTX_free(ctx);
    return ok;
}

bool Ed25519Signer::verify(std::string_view message, std::string_view sig_b64) const {
    if (key_ == nullptr) return false;

    std::vector<uint8_t> sig;
    if (!base64_decode(sig_b64, sig)) return false;

    EVP_MD_CTX* ctx = ::EVP_MD_CTX_new();
    if (ctx == nullptr) return false;

    bool ok = false;
    if (::EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key_) == 1) {
        ok = ::EVP_DigestVerify(ctx, sig.data(), sig.size(),
                                reinterpret_cast<const unsigned char*>(message.data()),
                                message.size()) == 1;
    }
    ::EVP_MD_CTX_free(ctx);
    return ok;
}

bool Ed25519Signer::public_key_pem(std::string& out) const {
    if (key_ == nullptr) return false;

    BIO* bio = ::BIO_new(::BIO_s_mem());
    if (bio == nullptr) return false;

    bool ok = ::PEM_write_bio_PUBKEY(bio, key_) == 1;
    if (ok) {
        char*     data = nullptr;
        const long n   = ::BIO_get_mem_data(bio, &data);
        out.assign(data, static_cast<size_t>(n));
    }
    ::BIO_free(bio);
    if (!ok) record("PEM_write_bio_PUBKEY");
    return ok;
}

bool Ed25519Signer::public_key_hex(std::string& out) const {
    if (key_ == nullptr) return false;

    uint8_t raw[32];
    size_t  len = sizeof(raw);
    if (::EVP_PKEY_get_raw_public_key(key_, raw, &len) != 1 || len != sizeof(raw)) {
        record("EVP_PKEY_get_raw_public_key");
        return false;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    out.clear();
    out.reserve(sizeof(raw) * 2);
    for (const uint8_t b : raw) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return true;
}

bool Ed25519Signer::private_key_pem(std::string& out) const {
    if (key_ == nullptr) return false;

    BIO* bio = ::BIO_new(::BIO_s_mem());
    if (bio == nullptr) return false;

    bool ok = ::PEM_write_bio_PKCS8PrivateKey(bio, key_, nullptr, nullptr, 0, nullptr,
                                              nullptr) == 1;
    if (ok) {
        char*     data = nullptr;
        const long n   = ::BIO_get_mem_data(bio, &data);
        out.assign(data, static_cast<size_t>(n));
    }
    ::BIO_free(bio);
    if (!ok) record("PEM_write_bio_PKCS8PrivateKey");
    return ok;
}

const char* Ed25519Signer::last_error() noexcept { return t_last_error; }

bool Credentials::from_env(Credentials& out, std::string& error) {
    const char* api_key  = std::getenv("LLT_BINANCE_API_KEY");
    const char* key_path = std::getenv("LLT_BINANCE_PRIVATE_KEY");

    if (api_key == nullptr || *api_key == '\0') {
        error = "LLT_BINANCE_API_KEY is not set";
        return false;
    }
    if (key_path == nullptr || *key_path == '\0') {
        error = "LLT_BINANCE_PRIVATE_KEY is not set (it must hold a path, not a key)";
        return false;
    }

    out.api_key = api_key;
    if (!out.signer.load_pem_file(key_path)) {
        error = Ed25519Signer::last_error();
        return false;
    }
    return true;
}

}  // namespace llt::exec
