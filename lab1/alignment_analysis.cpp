/*
 * alignment_analysis.cpp — Determine exact span-to-RIP alignment
 *
 * Replicates the REAL rleOpGeneric stack layout (ALL local variables
 * including the 1024-byte coverage buffer) and measures where each
 * span field lands relative to saved RIP, saved RBP, and local pointers.
 *
 * Build:
 *   g++ -O0 -fno-stack-protector -no-pie -o alignment_analysis alignment_analysis.cpp
 *   g++ -O2 -fno-stack-protector -no-pie -o alignment_analysis_o2 alignment_analysis.cpp
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <array>
#include <algorithm>

using uchar  = unsigned char;
using ushort = unsigned short;

struct Span {
    short  x{0};
    short  y{0};
    ushort len{0};
    uchar  coverage{0};
};

struct VRleHelper {
    Span  *spans;
    size_t size;
    size_t alloc;
};

enum class Operation { Add, Xor };

/*
 * Exact replica of rleOpGeneric's local variable set from vrle.cpp:613-676.
 * We keep ALL variables to get the same stack frame layout.
 * Variables are marked volatile to prevent optimization from removing them.
 */
__attribute__((noinline))
static void measure_real_layout(VRleHelper *a, VRleHelper *b, VRleHelper *result,
                                Operation op)
{
    // ---- Line 616-622: top-level locals ----
    std::array<Span, 256> temp;
    volatile Span  *out       = result->spans;
    volatile size_t available = result->alloc;
    volatile Span  *aPtr      = a->spans;
    volatile Span  *aEnd      = a->spans + a->size;
    volatile Span  *bPtr      = b->spans;
    volatile Span  *bEnd      = b->spans + b->size;

    // ---- Line 632-641: inner-loop locals (allocated at frame entry by compiler) ----
    volatile Span  *aStart    = nullptr;
    volatile Span  *bStart    = nullptr;
    volatile int    y_var     = 0;
    volatile int    aLength   = 0;
    volatile int    bLength   = 0;
    volatile int    offset    = 0;

    // ---- Line 644-645: the 1024-byte coverage buffer ----
    std::array<uchar, 1024> array;
    memset(array.data(), 0, 1024);

    // ---- Line 651-652: bufferToRle output vars ----
    volatile Span  *tResult   = temp.data();
    volatile size_t size_var  = 0;

    // ---- Now measure everything ----
    uintptr_t *rbp_ptr;
    asm volatile("mov %%rbp, %0" : "=r"(rbp_ptr));
    uintptr_t *rip_slot = rbp_ptr + 1;

    uintptr_t temp_addr    = (uintptr_t)temp.data();
    uintptr_t temp_end     = (uintptr_t)(temp.data() + 256);
    uintptr_t array_addr   = (uintptr_t)array.data();
    uintptr_t rip_addr     = (uintptr_t)rip_slot;
    uintptr_t rbp_addr     = (uintptr_t)rbp_ptr;

    ptrdiff_t dist_end_to_rip = (ptrdiff_t)(rip_addr - temp_end);
    int misalignment = (int)(dist_end_to_rip % 8);
    if (misalignment < 0) misalignment += 8;

    printf("=== Stack Layout (replicating real rleOpGeneric @ -O0) ===\n\n");
    printf("  temp[256] (2048B):    %p — %p\n", (void*)temp_addr, (void*)temp_end);
    printf("  array[1024]:          %p — %p\n", (void*)array_addr, (void*)(array_addr + 1024));
    printf("  saved RBP:            %p\n", (void*)rbp_addr);
    printf("  saved RIP:            %p\n", (void*)rip_addr);
    printf("\n");

    // Show all variable addresses
    printf("  Variable addresses (between temp and RIP):\n");
    printf("    &out:        %p  (offset from temp end: %+td)\n", (void*)&out, (ptrdiff_t)((uintptr_t)&out - temp_end));
    printf("    &available:  %p  (offset from temp end: %+td)\n", (void*)&available, (ptrdiff_t)((uintptr_t)&available - temp_end));
    printf("    &aPtr:       %p  (offset from temp end: %+td)\n", (void*)&aPtr, (ptrdiff_t)((uintptr_t)&aPtr - temp_end));
    printf("    &aEnd:       %p  (offset from temp end: %+td)\n", (void*)&aEnd, (ptrdiff_t)((uintptr_t)&aEnd - temp_end));
    printf("    &bPtr:       %p  (offset from temp end: %+td)\n", (void*)&bPtr, (ptrdiff_t)((uintptr_t)&bPtr - temp_end));
    printf("    &bEnd:       %p  (offset from temp end: %+td)\n", (void*)&bEnd, (ptrdiff_t)((uintptr_t)&bEnd - temp_end));
    printf("    &aStart:     %p  (offset from temp end: %+td)\n", (void*)&aStart, (ptrdiff_t)((uintptr_t)&aStart - temp_end));
    printf("    &bStart:     %p  (offset from temp end: %+td)\n", (void*)&bStart, (ptrdiff_t)((uintptr_t)&bStart - temp_end));
    printf("    &y_var:      %p  (offset from temp end: %+td)\n", (void*)&y_var, (ptrdiff_t)((uintptr_t)&y_var - temp_end));
    printf("    &aLength:    %p  (offset from temp end: %+td)\n", (void*)&aLength, (ptrdiff_t)((uintptr_t)&aLength - temp_end));
    printf("    &bLength:    %p  (offset from temp end: %+td)\n", (void*)&bLength, (ptrdiff_t)((uintptr_t)&bLength - temp_end));
    printf("    &offset:     %p  (offset from temp end: %+td)\n", (void*)&offset, (ptrdiff_t)((uintptr_t)&offset - temp_end));
    printf("    &array:      %p  (offset from temp end: %+td)\n", (void*)array.data(), (ptrdiff_t)((uintptr_t)array.data() - temp_end));
    printf("    &tResult:    %p  (offset from temp end: %+td)\n", (void*)&tResult, (ptrdiff_t)((uintptr_t)&tResult - temp_end));
    printf("    &size_var:   %p  (offset from temp end: %+td)\n", (void*)&size_var, (ptrdiff_t)((uintptr_t)&size_var - temp_end));
    printf("\n");

    printf("  Distance temp[256] -> RIP: %td bytes\n", dist_end_to_rip);
    printf("  Misalignment (mod 8):      %d bytes\n", misalignment);
    printf("  Overflow spans to RIP:     %td\n", dist_end_to_rip / 8);
    printf("  Total overflow spans:      256 (2048 bytes)\n");
    printf("  Spans past RIP:            %td\n", 256 - dist_end_to_rip / 8 - 1);
    printf("\n");

    // Byte-by-byte mapping for saved RIP
    const char *field_names[] = {"x_lo", "x_hi", "y_lo", "y_hi", "len_lo", "len_hi", "coverage", "pad=0x00"};
    const char *control[]     = {"FULL", "FULL", "SCANLINE", "SCANLINE", "FULL", "FULL", "1..255", "FORCED 0"};

    printf("=== RIP Byte ↔ Span Field Mapping ===\n\n");
    for (int i = 0; i < 8; i++) {
        int field = (int)((dist_end_to_rip + i) % 8);
        if (field < 0) field += 8;
        printf("  RIP[%d]  →  %-10s  control: %s\n", i, field_names[field], control[field]);
    }

    printf("\n=== RBP Byte ↔ Span Field Mapping ===\n\n");
    ptrdiff_t dist_end_to_rbp = (ptrdiff_t)(rbp_addr - temp_end);
    for (int i = 0; i < 8; i++) {
        int field = (int)((dist_end_to_rbp + i) % 8);
        if (field < 0) field += 8;
        printf("  RBP[%d]  →  %-10s  control: %s\n", i, field_names[field], control[field]);
    }

    // Check which local pointers get overwritten and how
    printf("\n=== Critical Pointer Corruption Analysis ===\n\n");

    struct { const char *name; uintptr_t addr; } ptrs[] = {
        {"out",      (uintptr_t)&out},
        {"available",(uintptr_t)&available},
        {"aPtr",     (uintptr_t)&aPtr},
        {"tResult",  (uintptr_t)&tResult},
    };

    for (auto &p : ptrs) {
        ptrdiff_t off = (ptrdiff_t)(p.addr - temp_end);
        if (off >= 0 && off < 2048) {
            int field_at_6 = (int)((off + 6) % 8);
            if (field_at_6 < 0) field_at_6 += 8;
            printf("  %-12s at offset %+4td  byte6→%-10s  %s\n",
                   p.name, off, field_names[field_at_6],
                   field_at_6 == 6 ? "NON-CANONICAL (coverage>0)" :
                   field_at_6 == 7 ? "CANONICAL OK (padding=0)" : "MAY WORK");
        } else if (off < 0) {
            printf("  %-12s at offset %+4td  BELOW temp end (not overwritten)\n", p.name, off);
        }
    }

    // Feasibility summary
    printf("\n=== Exploitation Feasibility Summary ===\n\n");

    int rip_field6 = (int)((dist_end_to_rip + 6) % 8);
    if (rip_field6 < 0) rip_field6 += 8;
    int rip_field7 = (int)((dist_end_to_rip + 7) % 8);
    if (rip_field7 < 0) rip_field7 += 8;

    bool rip_feasible = (rip_field6 != 6) && (rip_field7 != 6);
    bool rbp_feasible = true;
    {
        int f6 = (int)((dist_end_to_rbp + 6) % 8);
        if (f6 < 0) f6 += 8;
        int f7 = (int)((dist_end_to_rbp + 7) % 8);
        if (f7 < 0) f7 += 8;
        rbp_feasible = (f6 != 6) && (f7 != 6);
    }

    printf("  Direct RIP overwrite to canonical addr:  %s\n",
           rip_feasible ? "POSSIBLE" : "BLOCKED (coverage byte at position 6)");
    printf("  Stack pivot via RBP corruption:          %s\n",
           rbp_feasible ? "POSSIBLE" : "BLOCKED (same reason)");
    printf("  Denial of Service (crash):               ALWAYS POSSIBLE\n");
    printf("  Data pointer corruption:                 See pointer analysis above\n");

    if (!rip_feasible) {
        printf("\n  The coverage field (always 1-255) of the Span struct lands\n");
        printf("  on byte 6 of the return address. All x86-64 canonical\n");
        printf("  userspace addresses require byte 6 = 0x00 (bits 48-55 = 0).\n");
        printf("  Coverage > 0 forces bit 48+ high → non-canonical → #GP fault.\n");
        printf("\n");
        printf("  This is a FUNDAMENTAL constraint of the Span struct layout.\n");
        printf("  It blocks: direct RIP hijack, RBP stack pivot, and\n");
        printf("  corruption of any pointer to a canonical address.\n");
        printf("\n");
        printf("  Possible mitigations of this constraint:\n");
        printf("  1. Different compiler/flags may produce different alignment\n");
        printf("  2. Overflow into caller's frame (different variable layout)\n");
        printf("  3. Corrupt non-pointer data (loop counters, sizes) for DoS\n");
        printf("  4. Build-specific: temp not 8-aligned to frame → misalignment > 0\n");
    }

    printf("\n  Original RIP value: 0x%016lx\n", *(unsigned long*)rip_slot);
    printf("  (sanity check: this should be a return address in main)\n");

    fflush(stdout);
}

int main()
{
    printf("\n");
    Span a_span = {0, 0, 1024, 255};
    Span result_buf[512];
    VRleHelper a_h = {&a_span, 1, 1};
    VRleHelper b_h = {&a_span, 0, 0};
    VRleHelper r_h = {result_buf, 0, 512};
    measure_real_layout(&a_h, &b_h, &r_h, Operation::Xor);
    return 0;
}
