#pragma once

#include <FreeInkUIGfxRenderer.h>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "themes/BaseTheme.h"

namespace TouchHeaderBackButton {

constexpr int ICON_SIZE = 32;
// Keep navigation controls close to the divider while leaving the status row unchanged.
constexpr int TITLE_VERTICAL_OFFSET = 11;

struct Layout {
  Rect iconRect;
  Rect touchRect;
  int titleX;
};

Layout layout(const Rect& header);
int height(const ThemeMetrics& metrics, const MappedInputManager& input);
Rect headerRect(const GfxRenderer& renderer, const MappedInputManager& input);
Rect standardHeaderRect(const GfxRenderer& renderer);
Rect compactHeaderRect(const GfxRenderer& renderer);
bool wasTapped(const MappedInputManager& input, const Rect& header);
bool wasTapped(const MappedInputManager& input, const GfxRenderer& renderer);
// Space to leave right of a header's own controls (the Library's sort and search, for
// instance), so they clear the theme's battery indicator.
int controlRightInset();
void draw(GfxRenderer& renderer, const Rect& header, const char* title, bool readerContext, int rightReserve = 0,
          const char* subtitle = nullptr, int verticalOffset = TITLE_VERTICAL_OFFSET, bool showStatus = true);
void draw(GfxRenderer& renderer, freeink::ui::GfxRendererTarget& target, const Rect& header, const char* title,
          bool readerContext, int rightReserve = 0, const char* subtitle = nullptr,
          int verticalOffset = TITLE_VERTICAL_OFFSET, bool showStatus = true);
void drawCompact(GfxRenderer& renderer, const char* title, bool readerContext = false, bool showDate = false,
                 int verticalOffset = TITLE_VERTICAL_OFFSET);

}  // namespace TouchHeaderBackButton
