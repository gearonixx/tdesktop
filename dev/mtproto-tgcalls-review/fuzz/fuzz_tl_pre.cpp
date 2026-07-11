// libFuzzer harness for the MTProto *pre-auth handshake* TL deserializers.
//
// These are the boxed types a malicious server / MITM / MTProto proxy can push
// at the client BEFORE any auth key exists — the exact surface where MTPROTO-1
// (parseNotSecureResponse boundary) and MTPROTO-3 (server_DH_params_ok length)
// live. The original fuzz_tl.cpp covers the post-auth server-push surface; this
// one is dedicated to the plaintext handshake read path.
//
// Bytes -> aligned mtpPrime buffer bracketed by ASan red-zones -> boxed
// read(from,end). Any past-end read is caught; Expects/Assert route through
// base::assertion::fail -> null-deref -> ASan crash.

#include "scheme.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace base::assertion {
void log(const char *message, const char *file, int line) {
    (void)message; (void)file; (void)line;
}
} // namespace base::assertion

namespace {
template <typename T>
void tryRead(const mtpPrime *from, const mtpPrime *end) {
    T value;
    value.read(from, end);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    const uint8_t selector = data[0];
    ++data; --size;

    const size_t primes = size / sizeof(mtpPrime);
    if (primes == 0) return 0;
    std::vector<mtpPrime> buffer(primes);
    memcpy(buffer.data(), data, primes * sizeof(mtpPrime));
    const auto *from = buffer.data();
    const auto *end = from + primes;

    switch (selector % 10) {
    // Pre-auth plaintext handshake responses (server -> client). tl::boxed<>
    // consumes the constructor id from the buffer, matching the real read path
    // (readPQFakeReply / DcKeyCreator::readNotSecureResponse).
    case 0: tryRead<tl::boxed<MTPresPQ>>(from, end); break;                      // req_pq -> ResPQ
    case 1: tryRead<tl::boxed<MTPserver_DH_Params>>(from, end); break;           // req_DH_params -> Server_DH_Params
    case 2: tryRead<tl::boxed<MTPserver_DH_inner_data>>(from, end); break;       // decrypted DH inner
    case 3: tryRead<tl::boxed<MTPset_client_DH_params_answer>>(from, end); break;// set_client_DH_params -> answer
    // Handshake inner data structures (round-trip read path):
    case 4: tryRead<tl::boxed<MTPclient_DH_Inner_Data>>(from, end); break;
    case 5: tryRead<tl::boxed<MTPp_Q_inner_data>>(from, end); break;
    // A few recursive/complex server-push types for depth coverage:
    case 6: tryRead<MTPUpdates>(from, end); break;
    case 7: tryRead<MTPPage>(from, end); break;                      // recursive (IV)
    case 8: tryRead<MTPRichText>(from, end); break;                  // recursive
    case 9: tryRead<MTPhelp_ConfigSimple>(from, end); break;        // config fetch path
    }
    return 0;
}
