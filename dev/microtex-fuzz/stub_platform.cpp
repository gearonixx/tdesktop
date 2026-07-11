// Minimal platform stub for MicroTeX so parse + layout (metrics) can run
// headless without Qt. We never call draw(), so glyph rasterization is not
// needed; only Font/TextLayout factory functions must be provided because
// core references them (font_info.cpp, box_single.cpp).
#include "graphic/graphic.h"
#include "graphic/graphic_basic.h"

using namespace tex;
using namespace std;

namespace {

class StubFont : public Font {
  float _size;
public:
  explicit StubFont(float size) : _size(size) {}
  float getSize() const override { return _size; }
  sptr<Font> deriveFont(int) const override { return sptrOf<StubFont>(_size); }
  bool operator==(const Font& f) const override { return this == &f; }
  bool operator!=(const Font& f) const override { return !(*this == f); }
};

class StubTextLayout : public TextLayout {
  std::wstring _src;
public:
  StubTextLayout(const std::wstring& s) : _src(s) {}
  // Provide a plausible non-zero bounds derived from length so layout math
  // has finite, deterministic inputs (exercises the same code paths).
  void getBounds(Rect& b) override {
    b.x = 0;
    b.y = -7.f;
    b.w = float(_src.size()) * 5.f;
    b.h = 10.f;
  }
  void draw(Graphics2D&, float, float) override {}
};

} // namespace

Font* Font::create(const string&, float size) {
  return new StubFont(size);
}

sptr<Font> Font::_create(const string&, int, float size) {
  return sptrOf<StubFont>(size);
}

sptr<TextLayout> TextLayout::create(const std::wstring& src, const sptr<Font>&) {
  return sptrOf<StubTextLayout>(src);
}
