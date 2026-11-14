#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;
struct RecentBook;
struct Rect;
namespace freeink {
struct Icon;
}

// The drawing the BookOrbit Home's pages share: tile frames and headers, big numbers,
// bars and charts, covers, and the title row over each page.
class BookOrbitHomeTiles {
 public:
  explicit BookOrbitHomeTiles(GfxRenderer& renderer) : renderer(renderer) {}

  // Empty when the book has no cover file to draw.
  static std::string coverPath(const RecentBook& book, int width, int height);

  // The page's title, then a dot per page with the current one filled.
  void drawTitleRow(const char* title, int pageCount, int currentPage) const;

  void drawFitted(int fontId, int x, int y, const char* text, int maxWidth, bool bold = false, bool black = true) const;
  void drawRightAligned(int fontId, int right, int y, const char* text, bool bold = false) const;
  void drawTileFrame(const Rect& tile) const;
  void drawTileHeader(const Rect& tile, const char* title, const freeink::Icon* icon = nullptr) const;
  void drawTileFooter(const Rect& tile, const char* text) const;
  void drawDot(int left, int top, int radius, bool filled) const;
  void drawBigValue(const Rect& tile, const char* value, const char* unit) const;
  void drawBar(int x, int base, int width, int chartHeight, uint32_t value, uint32_t highest, bool emphasized) const;
  void drawProgressBar(int x, int y, int width, int height, float fraction, bool black = true) const;
  void drawColumnChart(const Rect& area, const uint32_t* values, int count, int emphasized, const char* initials) const;
  // A framed tile of values over their labels.
  void drawMetrics(const Rect& tile, const char* const* values, const char* const* labels, int count) const;
  void drawMetricColumns(const Rect& tile, int valueY, int ruleTop, int ruleBottom, const char* const* values,
                         const char* const* labels, int count) const;
  void drawCover(const RecentBook& book, const Rect& slot, bool onBlack) const;
  void drawCoverFile(const char* path, const Rect& slot, bool onBlack) const;

 private:
  GfxRenderer& renderer;
};
