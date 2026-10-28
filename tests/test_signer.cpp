#include "harness.hpp"
#include "llt/exec/signer.hpp"

#include <cstdio>
#include <string>

using llt::exec::Ed25519Signer;

namespace {

// RFC 8032 section 7.1, test 1. The seed and the public key it must produce.
constexpr uint8_t kRfcSeed[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
    0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
    0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

constexpr const char* kRfcPublicHex =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

}  // namespace

LLT_TEST(signer_is_unusable_before_a_key_is_loaded) {
    Ed25519Signer s;
    LLT_CHECK(!s.valid());

    std::string sig;
    LLT_CHECK(!s.sign("anything", sig));
}

LLT_TEST(signer_derives_the_rfc8032_public_key) {
    Ed25519Signer s;
    LLT_CHECK(s.load_seed(kRfcSeed));
    LLT_CHECK(s.valid());

    std::string hex;
    LLT_CHECK(s.public_key_hex(hex));
    LLT_CHECK(hex == kRfcPublicHex);
}

LLT_TEST(signer_round_trips_sign_and_verify) {
    Ed25519Signer s;
    LLT_CHECK(s.generate());

    const std::string message = "symbol=BTCUSDT&side=BUY&timestamp=1793140093437";

    std::string sig;
    LLT_CHECK(s.sign(message, sig));
    LLT_CHECK(!sig.empty());
    LLT_CHECK(s.verify(message, sig));
}

LLT_TEST(signer_rejects_a_tampered_message) {
    Ed25519Signer s;
    LLT_CHECK(s.generate());

    std::string sig;
    LLT_CHECK(s.sign("side=BUY&quantity=1", sig));

    // One character different is a different order.
    LLT_CHECK(!s.verify("side=BUY&quantity=2", sig));
    LLT_CHECK(!s.verify("side=SELL&quantity=1", sig));
}

LLT_TEST(signer_rejects_a_tampered_signature) {
    Ed25519Signer s;
    LLT_CHECK(s.generate());

    const std::string message = "side=BUY";
    std::string       sig;
    LLT_CHECK(s.sign(message, sig));

    std::string broken = sig;
    broken[0] = (broken[0] == 'A') ? 'B' : 'A';
    LLT_CHECK(!s.verify(message, broken));
}

LLT_TEST(signer_is_deterministic) {
    Ed25519Signer s;
    LLT_CHECK(s.load_seed(kRfcSeed));

    // Ed25519 derives its nonce from the key and message, so the same input always
    // produces the same signature. A differing result would mean entropy leaking in
    // where the standard says none should.
    std::string a;
    std::string b;
    LLT_CHECK(s.sign("timestamp=1", a));
    LLT_CHECK(s.sign("timestamp=1", b));
    LLT_CHECK(a == b);

    std::string c;
    LLT_CHECK(s.sign("timestamp=2", c));
    LLT_CHECK(a != c);
}

LLT_TEST(signer_pem_round_trip_preserves_the_key) {
    Ed25519Signer original;
    LLT_CHECK(original.generate());

    std::string pem;
    LLT_CHECK(original.private_key_pem(pem));
    LLT_CHECK(pem.find("BEGIN PRIVATE KEY") != std::string::npos);

    Ed25519Signer reloaded;
    LLT_CHECK(reloaded.load_pem(pem));

    std::string a;
    std::string b;
    LLT_CHECK(original.public_key_hex(a));
    LLT_CHECK(reloaded.public_key_hex(b));
    LLT_CHECK(a == b);

    // A signature from one must verify against the other.
    std::string sig;
    LLT_CHECK(original.sign("hello", sig));
    LLT_CHECK(reloaded.verify("hello", sig));
}

LLT_TEST(signer_public_pem_is_a_public_key) {
    Ed25519Signer s;
    LLT_CHECK(s.generate());

    std::string pem;
    LLT_CHECK(s.public_key_pem(pem));
    LLT_CHECK(pem.find("BEGIN PUBLIC KEY") != std::string::npos);
    // The half that gets uploaded must never carry private material.
    LLT_CHECK(pem.find("PRIVATE") == std::string::npos);
}

LLT_TEST(signer_refuses_a_non_ed25519_key) {
    // A valid PEM of the wrong type must be rejected rather than half-loaded.
    const char* rsa_ish =
        "-----BEGIN PRIVATE KEY-----\nbm90IGEgcmVhbCBrZXk=\n-----END PRIVATE KEY-----\n";

    Ed25519Signer s;
    LLT_CHECK(!s.load_pem(rsa_ish));
    LLT_CHECK(!s.valid());
}

LLT_TEST(signer_reports_a_missing_key_file) {
    Ed25519Signer s;
    LLT_CHECK(!s.load_pem_file("/tmp/llt_definitely_not_a_key_file.pem"));
    LLT_CHECK(!s.valid());
}
