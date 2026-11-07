#pragma once
#include <Epub/blocks/ImageBlock.h>
#include <GfxRenderer.h>

#include <vector>
class Page {
 public:
  struct PlacedImage {
    ImageBlock* image;
    int x, y;
  };
  std::vector<PlacedImage> images;
  mutable int allVisits = 0, imageVisits = 0;
  void renderImages(GfxRenderer& renderer, int, int x, int y) const {
    ++imageVisits;
    for (auto& item : images) item.image->render(renderer, x + item.x, y + item.y, true);
  }
  void renderText(GfxRenderer&, int, int, int, bool) const { ++allVisits; }
  void render(GfxRenderer& renderer, int font, int x, int y, bool black) const {
    renderText(renderer, font, x, y, black);
    renderImages(renderer, font, x, y);
  }
};
