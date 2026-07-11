// libFuzzer harness for MicroTeX LaTeX::parse (the IV markdown math engine).
// Mirrors iv_markdown_microtex.cpp: it feeds attacker-controlled LaTeX through
// LaTeX::parse(), which runs the full parser + macro expansion + atom-tree
// layout (createBox). We catch C++ exceptions (normal MicroTeX error control
// flow) so only ASan/UBSan faults surface as crashes.
#include "latex.h"
#include "core/formula.h"

#include <cstdint>
#include <string>
#include <exception>

using namespace tex;

// Decode UTF-8 input into a wstring of Unicode code points (wchar_t is 32-bit
// on Linux), so the fuzzer can reach both ASCII command paths and the
// non-ASCII / surrogate handling in the parser.
static std::wstring toWide(const uint8_t* data, size_t size) {
  std::wstring out;
  out.reserve(size);
  size_t i = 0;
  while (i < size) {
    uint32_t c = data[i];
    if (c < 0x80) {
      out.push_back((wchar_t)c);
      i += 1;
    } else if ((c >> 5) == 0x6 && i + 1 < size) {
      uint32_t cp = ((c & 0x1F) << 6) | (data[i + 1] & 0x3F);
      out.push_back((wchar_t)cp);
      i += 2;
    } else if ((c >> 4) == 0xE && i + 2 < size) {
      uint32_t cp = ((c & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6)
                    | (data[i + 2] & 0x3F);
      out.push_back((wchar_t)cp);
      i += 3;
    } else if ((c >> 3) == 0x1E && i + 3 < size) {
      uint32_t cp = ((c & 0x07) << 18) | ((data[i + 1] & 0x3F) << 12)
                    | ((data[i + 2] & 0x3F) << 6) | (data[i + 3] & 0x3F);
      out.push_back((wchar_t)cp);
      i += 4;
    } else {
      out.push_back((wchar_t)c);
      i += 1;
    }
  }
  return out;
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  LaTeX::initBundled();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Bound input length like the real pipeline (formulas are small spans).
  if (size == 0 || size > 8192) return 0;
  std::wstring latex = toWide(data, size);
  try {
    // Match the wrapper: metric render width cap, text size, line space, fg.
    TeXRender* render = LaTeX::parse(latex, 720, 20.f, 20.f / 3.f, 0xFF000000);
    delete render;
  } catch (const std::exception&) {
  } catch (...) {
  }
  return 0;
}
