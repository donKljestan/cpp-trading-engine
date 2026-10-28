#pragma once

// Ed25519 request signing.
//
// Binance supports Ed25519 keys, which matters for two reasons. The private key never
// leaves this machine - only the public half is uploaded - and the WebSocket API can
// authenticate once per session instead of signing every order.
//
// Nothing here ever prints, logs or copies a private key.

#include <cstdint>
#include <string>
#include <string_view>

using EVP_PKEY = struct evp_pkey_st;

namespace llt::exec {

class Ed25519Signer {
public:
    Ed25519Signer() = default;
    ~Ed25519Signer();

    Ed25519Signer(Ed25519Signer&& other) noexcept;
    Ed25519Signer& operator=(Ed25519Signer&& other) noexcept;
    Ed25519Signer(const Ed25519Signer&)            = delete;
    Ed25519Signer& operator=(const Ed25519Signer&) = delete;

    [[nodiscard]] bool load_pem_file(const std::string& path);
    [[nodiscard]] bool load_pem(std::string_view pem);

    // Raw 32-byte seed. Exists for test vectors; production keys arrive as PEM.
    [[nodiscard]] bool load_seed(const uint8_t seed[32]);

    [[nodiscard]] bool generate();

    // Base64 of the 64-byte signature, which is the encoding Binance expects.
    [[nodiscard]] bool sign(std::string_view message, std::string& out_b64) const;

    [[nodiscard]] bool verify(std::string_view message, std::string_view sig_b64) const;

    // Safe to display: this is the half that gets uploaded to the exchange.
    [[nodiscard]] bool public_key_pem(std::string& out) const;
    [[nodiscard]] bool public_key_hex(std::string& out) const;

    // PKCS#8 PEM of the private key. Only the key generator calls this, and it writes
    // straight to a file with restrictive permissions.
    [[nodiscard]] bool private_key_pem(std::string& out) const;

    [[nodiscard]] bool valid() const noexcept { return key_ != nullptr; }

    [[nodiscard]] static const char* last_error() noexcept;

private:
    void reset() noexcept;

    EVP_PKEY* key_ = nullptr;
};

// Everything needed to authenticate, assembled from the environment.
//
//   LLT_BINANCE_API_KEY        the key id shown in the exchange UI
//   LLT_BINANCE_PRIVATE_KEY    path to the PEM file, not the key itself
//
// The path rather than the key, because an environment variable holding key material
// is visible in /proc and in any crash dump that captures the environment.
struct Credentials {
    std::string   api_key;
    Ed25519Signer signer;

    [[nodiscard]] static bool from_env(Credentials& out, std::string& error);
};

}  // namespace llt::exec
