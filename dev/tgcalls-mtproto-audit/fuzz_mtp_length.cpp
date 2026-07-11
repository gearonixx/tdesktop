// Harness for the MTProto TCP framing length parsers (connection_tcp.cpp
// Version0 abridged + VersionD), reproduced verbatim, checking the invariant
// the caller relies on: a returned positive size must satisfy
// sizeLength <= size <= bytes.size() so readPacket()'s subspan never goes OOB.
// This is the proxy/MITM-facing seam (obfuscated transport).
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <vector>
#include <cassert>

static constexpr int kUnknownSize = -1;
static constexpr int kInvalidSize = -2;
static constexpr int kPacketSizeMax = int(0x01000000 * 4);

// --- Version0 (abridged / not-obfuscated) ---
static int v0_len(const uint8_t *b, size_t n) {
    if (n == 0) return kUnknownSize;
    const auto first = static_cast<char>(b[0]);
    if (first == 0x7F) {
        if (n < 4) return kUnknownSize;
        const auto ints = uint32_t(b[1]) | (uint32_t(b[2]) << 8) | (uint32_t(b[3]) << 16);
        return (ints >= 0x7F) ? (int(ints << 2) + 4) : kInvalidSize;
    } else if (first > 0 && first < 0x7F) {
        const auto ints = uint32_t(first);
        return int(ints << 2) + 1;
    }
    return kInvalidSize;
}

// --- VersionD (obfuscated, dd/ee secret) ---
static int vd_len(const uint8_t *b, size_t n) {
    if (n < 4) return kUnknownSize;
    uint32_t raw; __builtin_memcpy(&raw, b, 4);
    const auto value = raw + 4;
    return (value >= 8 && value < uint32_t(kPacketSizeMax)) ? int(value) : kInvalidSize;
}

static void check(int size, size_t n, int sizeLength) {
    if (size == kUnknownSize || size == kInvalidSize) return;
    // Caller Asserts size <= bytes.size() only once enough bytes arrived; but the
    // subspan(sizeLength, size - sizeLength) must never underflow/overflow.
    assert(size > 0);
    assert(size >= sizeLength);           // else size - sizeLength underflows (huge)
    assert(size - sizeLength >= 0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t s) {
    int a = v0_len(d, s);
    check(a, s, (s && (char)d[0] == 0x7F) ? 4 : 1);
    int b = vd_len(d, s);
    check(b, s, 4);
    return 0;
}
