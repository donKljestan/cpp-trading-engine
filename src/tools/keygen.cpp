// Generates an Ed25519 key pair for exchange authentication.
//
// The private key is written straight to a file with owner-only permissions and is
// never printed. Only the public half goes to the screen, because only the public half
// goes to the exchange.
//
//   keygen ~/.llt/binance_testnet.pem

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "llt/exec/signer.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: keygen PATH_FOR_PRIVATE_KEY.pem\n");
        return 1;
    }
    const std::string path = argv[1];

    // Refusing to overwrite: a silently replaced key means orders start being rejected
    // with no obvious cause.
    if (std::ifstream(path).good()) {
        std::fprintf(stderr, "%s already exists. refusing to overwrite a private key.\n",
                     path.c_str());
        return 1;
    }

    llt::exec::Ed25519Signer signer;
    if (!signer.generate()) {
        std::fprintf(stderr, "key generation failed: %s\n",
                     llt::exec::Ed25519Signer::last_error());
        return 1;
    }

    std::string private_pem;
    std::string public_pem;
    std::string public_hex;
    if (!signer.private_key_pem(private_pem) || !signer.public_key_pem(public_pem) ||
        !signer.public_key_hex(public_hex)) {
        std::fprintf(stderr, "could not export the key: %s\n",
                     llt::exec::Ed25519Signer::last_error());
        return 1;
    }

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            return 1;
        }
        out << private_pem;
    }
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::fprintf(stderr, "warning: could not restrict permissions on %s\n",
                     path.c_str());
    }

    std::printf("private key written to %s (mode 600)\n", path.c_str());
    std::printf("public key fingerprint  %s\n\n", public_hex.c_str());
    std::printf("paste this public key into the exchange when creating the API key:\n\n");
    std::printf("%s\n", public_pem.c_str());
    std::printf("then export, ideally from a file that is not in this repository:\n");
    std::printf("  export LLT_BINANCE_API_KEY=<the key id the exchange shows you>\n");
    std::printf("  export LLT_BINANCE_PRIVATE_KEY=%s\n", path.c_str());

    return 0;
}
