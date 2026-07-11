// PoC for F-2: MTProto plaintext-handshake parseNotSecureResponse OOB read.
// Replicates the exact length logic on a heap buffer. The consumer walks the
// returned span (as the TL deserializer does via response.read(from, from+size)).
// BUGGY variant (answerLen as element count) reads past the heap allocation;
// FIXED variant (answerLen / 4) stays in bounds. Build with ASan.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

using mtpPrime = int32_t;

// Returns [ptr, count] span the deserializer would iterate over.
static std::pair<const mtpPrime*, size_t> parse(const std::vector<mtpPrime>& buf, bool fixed) {
    const mtpPrime* answer = buf.data();
    const size_t len = buf.size();
    if (len < 6) return {nullptr, 0};
    if (answer[0] != 0 || answer[1] != 0 || (((uint32_t)answer[2]) & 0x03) != 1)
        return {nullptr, 0};
    const uint32_t answerLen = (uint32_t)answer[4];               // BYTE length
    if (answerLen < 1 || answerLen > (len - 5) * sizeof(mtpPrime)) // bounds as bytes -> passes
        return {nullptr, 0};
    const size_t count = fixed ? (answerLen / sizeof(mtpPrime))    // FIX: bytes -> primes
                               : answerLen;                        // BUG: bytes used as prime count
    return {answer + 5, count};
}

// Simulates the TL read walking every prime in the span.
static long consume(const std::vector<mtpPrime>& buf, bool fixed) {
    auto [p, n] = parse(buf, fixed);
    long acc = 0;
    for (size_t i = 0; i < n; ++i) acc += p[i];  // OOB here in buggy variant
    return acc;
}

int main(int argc, char** argv) {
    const bool fixed = (argc > 1 && std::strcmp(argv[1], "fixed") == 0);

    // Craft a minimal, VALID-looking ResPQ envelope of, say, 16 primes total.
    // header = 5 primes; payload = 11 primes = 44 bytes. Set answerLen = 44 (max
    // allowed by the byte-bound check). Buggy code then makes a span of 44
    // *elements*, walking 44 primes from offset 5 => reads primes [5..49) while
    // the heap vector holds only 16 => 33 primes OOB.
    std::vector<mtpPrime> buf(16, 0);
    buf[0] = 0; buf[1] = 0; buf[2] = 1;      // start markers (mod4 == 1)
    buf[3] = 0;                              // (msg id / time, unchecked)
    buf[4] = (mtpPrime)((16 - 5) * sizeof(mtpPrime)); // answerLen = 44 bytes

    printf("variant=%s buffer=%zu primes\n", fixed ? "FIXED" : "BUGGY", buf.size());
    volatile long r = consume(buf, fixed);
    printf("consumed ok, acc=%ld\n", (long)r);
    return 0;
}
