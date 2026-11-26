#include "BookOrbitCatalogScreens.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace BookOrbitCatalogScreens {
namespace {
// The one hint the screens around the listing show, under the first button.
void drawFirstHint(GfxRenderer& renderer, const MappedInputManager& input, const char* label) {
  const auto labels = input.mapLabels(label, "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
}  // namespace

void drawHeader(GfxRenderer& renderer, const MappedInputManager& input, const char* title, const bool listControls) {
  if (input.hasTouchHardware() && listControls) {
    // The title stops short of the sort and filter controls.
    TouchHeaderBackButton::draw(
        renderer, TouchHeaderBackButton::compactHeaderRect(renderer), title, /*readerContext=*/false,
        2 * HEADER_CONTROL_SIZE + HEADER_CONTROL_GAP + TouchHeaderBackButton::controlRightInset() + 10);
  } else if (input.hasTouchHardware()) {
    TouchHeaderBackButton::drawCompact(renderer, title);
  } else {
    CompactHeader::drawTitle(renderer, title);
  }
}

void drawStatus(GfxRenderer& renderer, const MappedInputManager& input, const char* message) {
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  drawFirstHint(renderer, input, tr(STR_BACK));
}

void drawError(GfxRenderer& renderer, const MappedInputManager& input, const char* message) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 50, tr(STR_ERROR_MSG));
  // Error messages can be several lines long; drawCenteredText with an over-wide
  // string starts at a negative x and clips, so wrap it explicitly.
  const int messageWidth = pageWidth - 80;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, message, messageWidth, 4);
  int messageY = pageHeight / 2 - 20;
  for (const auto& line : messageLines) {
    renderer.drawCenteredText(UI_10_FONT_ID, messageY, line.c_str());
    messageY += lineHeight + 4;
  }
  drawFirstHint(renderer, input, tr(STR_BACK));
}

void drawDownload(GfxRenderer& renderer, const MappedInputManager& input, const char* title, const size_t downloaded,
                  const size_t total) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title);
  if (total > 0) {
    GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloaded, total);
  }
  drawFirstHint(renderer, input, tr(STR_CANCEL));
}

void drawListHints(GfxRenderer& renderer, const MappedInputManager& input, const char* confirmLabel,
                   const bool listButtons) {
  const auto labels = input.mapLabels(tr(STR_BACK), confirmLabel, listButtons ? tr(STR_SORT) : "",
                                      listButtons ? tr(STR_BOOKORBIT_FILTER) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

}  // namespace BookOrbitCatalogScreens
