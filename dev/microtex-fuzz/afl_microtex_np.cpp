// AFL++ NON-persistent harness: deferred fork server after initBundled, then
// ONE parse per forked child. Every crash reproduces as a single input (no
// cross-iteration state), which matches the real app (LaTeX::parse resets
// global macro/matrix state on each call) and yields clean, deterministic PoCs.
#include "latex.h"
#include "core/formula.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <exception>
#include <unistd.h>

using namespace tex;

static std::wstring toWide(const uint8_t* data, size_t size) {
  std::wstring out; out.reserve(size);
  size_t i = 0;
  while (i < size) {
    uint32_t c = data[i];
    if (c < 0x80) { out.push_back((wchar_t)c); i += 1; }
    else if ((c >> 5) == 0x6 && i + 1 < size) {
      out.push_back((wchar_t)(((c & 0x1F) << 6) | (data[i+1] & 0x3F))); i += 2;
    } else if ((c >> 4) == 0xE && i + 2 < size) {
      out.push_back((wchar_t)(((c & 0x0F) << 12) | ((data[i+1] & 0x3F) << 6) | (data[i+2] & 0x3F))); i += 3;
    } else if ((c >> 3) == 0x1E && i + 3 < size) {
      out.push_back((wchar_t)(((c & 0x07) << 18) | ((data[i+1] & 0x3F) << 12) | ((data[i+2] & 0x3F) << 6) | (data[i+3] & 0x3F))); i += 4;
    } else { out.push_back((wchar_t)c); i += 1; }
  }
  return out;
}

static void runOne(const uint8_t* data, size_t size) {
  if (size == 0 || size > 8192) return;
  std::wstring latex = toWide(data, size);
  try {
    TeXRender* render = LaTeX::parse(latex, 720, 20.f, 20.f / 3.f, 0xFF000000);
    delete render;
  } catch (const std::exception&) {
  } catch (...) {
  }
}

#ifdef __AFL_HAVE_MANUAL_CONTROL
#include <cstdlib>
#endif

int main(int argc, char** argv) {
  LaTeX::initBundled();
#ifdef __AFL_HAVE_MANUAL_CONTROL
  __AFL_INIT();   // deferred fork server: children fork here, post-init
#endif
  std::vector<uint8_t> b;
  if (argc >= 2) {
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { b.resize(n); size_t got = fread(b.data(), 1, n, f); b.resize(got); }
    fclose(f);
  } else {
    uint8_t tmp[8192]; size_t got = fread(tmp, 1, sizeof(tmp), stdin);
    b.assign(tmp, tmp + got);
  }
  runOne(b.data(), b.size());
  return 0;
}
