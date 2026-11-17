#include "BookOrbitSyncScreens.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

namespace BookOrbitSyncScreens {
namespace {
constexpr int RESULT_NON_TOUCH_ACTION_HEIGHT = 40;
constexpr int RESULT_NON_TOUCH_ACTION_GAP = 8;
constexpr int RESULT_CARD_PADDING = 12;
constexpr int RESULT_CARD_ROW_GAP = 6;
constexpr int RESULT_CARD_BAR_HEIGHT = 10;
constexpr int RESULT_BADGE_PADDING_X = 6;

// Progress in tenths of a percent, matching the card's "%.1f%%" label so the "Ahead" badge never
// disagrees with the numbers shown on screen.
long displayedTenths(const float percentage) { return std::lround(percentage * 1000.0f); }

int progressCardHeight(const GfxRenderer& renderer) {
  return RESULT_CARD_PADDING * 2 + renderer.getLineHeight(UI_10_FONT_ID) + RESULT_CARD_ROW_GAP +
         renderer.getLineHeight(UI_12_FONT_ID) + RESULT_CARD_ROW_GAP + RESULT_CARD_BAR_HEIGHT + RESULT_CARD_ROW_GAP +
         renderer.getLineHeight(UI_10_FONT_ID);
}

// Draws text so its baseline lines up with a larger font drawn at rowY.
void drawTextOnBaseline(const GfxRenderer& renderer, const int fontId, const int x, const int rowY, const int rowFontId,
                        const char* text, const bool black = true,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int y = rowY + renderer.getFontAscenderSize(rowFontId) - renderer.getFontAscenderSize(fontId);
  renderer.drawText(fontId, x, y, text, black, style);
}

// Card layout:
//   Title  source                  [Ahead]
//   42.5%                        Page 3/12
//   [=========                          ]
//   Chapter name...
void drawProgressCard(const GfxRenderer& renderer, const Rect& card, const ProgressCard& info, const bool ahead) {
  renderer.drawRect(card.x, card.y, card.width, card.height, true);

  const int x = card.x + RESULT_CARD_PADDING;
  const int innerWidth = std::max(1, card.width - RESULT_CARD_PADDING * 2);
  const int right = x + innerWidth;
  const int smallGap = RESULT_CARD_ROW_GAP * 2;
  int y = card.y + RESULT_CARD_PADDING;

  // Row 1: title, optional source, optional "Ahead" badge.
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int titleRight = right;
  if (ahead) {
    const char* badge = tr(STR_SYNC_AHEAD);
    const int badgeWidth = renderer.getTextWidth(SMALL_FONT_ID, badge) + RESULT_BADGE_PADDING_X * 2;
    const int badgeX = right - badgeWidth;
    renderer.fillRect(badgeX, y, badgeWidth, titleLineHeight, true);
    const int badgeTextY = y + (titleLineHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, badgeX + RESULT_BADGE_PADDING_X, badgeTextY, badge, false);
    titleRight = badgeX - smallGap;
  }
  const std::string title =
      renderer.truncatedText(UI_10_FONT_ID, info.title, std::max(1, titleRight - x), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  if (info.source != nullptr && info.source[0] != '\0') {
    const int sourceX = x + renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), EpdFontFamily::BOLD) + smallGap;
    if (titleRight - sourceX > 0) {
      const std::string source = renderer.truncatedText(SMALL_FONT_ID, info.source, titleRight - sourceX);
      drawTextOnBaseline(renderer, SMALL_FONT_ID, sourceX, y, UI_10_FONT_ID, source.c_str());
    }
  }
  y += titleLineHeight + RESULT_CARD_ROW_GAP;

  // Row 2: large percentage with the page position right-aligned on the same baseline.
  char percentStr[16];
  snprintf(percentStr, sizeof(percentStr), "%.1f%%", info.percentage * 100);
  renderer.drawText(UI_12_FONT_ID, x, y, percentStr, true, EpdFontFamily::BOLD);
  const int percentRight = x + renderer.getTextWidth(UI_12_FONT_ID, percentStr, EpdFontFamily::BOLD) + smallGap;
  if (right - percentRight > 0) {
    const std::string page = renderer.truncatedText(UI_10_FONT_ID, info.pageText, right - percentRight);
    const int pageX = right - renderer.getTextWidth(UI_10_FONT_ID, page.c_str());
    drawTextOnBaseline(renderer, UI_10_FONT_ID, pageX, y, UI_12_FONT_ID, page.c_str());
  }
  y += renderer.getLineHeight(UI_12_FONT_ID) + RESULT_CARD_ROW_GAP;

  // Row 3: progress bar.
  renderer.drawRect(x, y, innerWidth, RESULT_CARD_BAR_HEIGHT, true);
  const float clamped = std::min(1.0f, std::max(0.0f, info.percentage));
  const int fillWidth = static_cast<int>((innerWidth - 4) * clamped);
  if (fillWidth > 0) {
    renderer.fillRect(x + 2, y + 2, fillWidth, RESULT_CARD_BAR_HEIGHT - 4, true);
  }
  y += RESULT_CARD_BAR_HEIGHT + RESULT_CARD_ROW_GAP;

  // Row 4: chapter name.
  const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, info.chapter, innerWidth);
  renderer.drawText(UI_10_FONT_ID, x, y, chapter.c_str());
}
}  // namespace

TouchActionButtons::Layout resultActionLayout(const Rect& screen, const ThemeMetrics& metrics, const bool hasTouch) {
  constexpr uint8_t buttonCount = 2;
  const int buttonHeight = hasTouch ? TouchActionButtons::kDefaultHeight : RESULT_NON_TOUCH_ACTION_HEIGHT;
  const int buttonGap = hasTouch ? TouchActionButtons::kDefaultGap : RESULT_NON_TOUCH_ACTION_GAP;
  // Touch devices spend the button-hint band on content: nothing reads it there.
  const int reservedBottom = hasTouch ? metrics.verticalSpacing : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int totalHeight = buttonHeight * buttonCount + buttonGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding, screen.y + screen.height - reservedBottom - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount, buttonHeight, buttonGap);
}

TouchActionButtons::Layout noRemoteProgressActionLayout(const Rect& screen, const ThemeMetrics& metrics) {
  constexpr uint8_t buttonCount = 2;
  constexpr int totalHeight =
      TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}

void drawStatus(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, const int top,
                const char* status, const std::string (&summaryLines)[2], const size_t progressDone,
                const size_t progressTotal) {
  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, status, true, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int lineY = top;
  for (const std::string& line : summaryLines) {
    if (line.empty()) continue;
    lineY += lineHeight + metrics.verticalSpacing;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, lineY, line.c_str());
  }
  if (progressTotal > 0) {
    char counts[32];
    snprintf(counts, sizeof(counts), "%u / %u", (unsigned)progressDone, (unsigned)progressTotal);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, counts);
    const int barWidth = screen.width / 2;
    const int barY = top + 2 * (lineHeight + metrics.verticalSpacing);
    GUI.drawProgressBar(renderer, Rect{screen.x + (screen.width - barWidth) / 2, barY, barWidth, 16}, progressDone,
                        progressTotal);
  }
}

void drawResult(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, const int top,
                const ProgressCard& remote, const ProgressCard& local, const int selectedOption, const bool hasTouch) {
  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

  const long remoteTenths = displayedTenths(remote.percentage);
  const long localTenths = displayedTenths(local.percentage);
  const bool remoteAhead = remoteTenths > localTenths;
  const bool localAhead = localTenths > remoteTenths;

  // Stack the cards in portrait; place them side by side in landscape where height is scarce.
  const int contentX = screen.x + metrics.contentSidePadding;
  const int contentWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
  const int cardHeight = progressCardHeight(renderer);
  const int cardGap = metrics.verticalSpacing;
  const int cardsTop = top + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  if (screen.width > screen.height) {
    const int cardWidth = std::max(1, (contentWidth - cardGap) / 2);
    drawProgressCard(renderer, Rect{contentX, cardsTop, cardWidth, cardHeight}, remote, remoteAhead);
    drawProgressCard(renderer, Rect{contentX + contentWidth - cardWidth, cardsTop, cardWidth, cardHeight}, local,
                     localAhead);
  } else {
    drawProgressCard(renderer, Rect{contentX, cardsTop, contentWidth, cardHeight}, remote, remoteAhead);
    drawProgressCard(renderer, Rect{contentX, cardsTop + cardHeight + cardGap, contentWidth, cardHeight}, local,
                     localAhead);
  }

  const auto actions = resultActionLayout(screen, metrics, hasTouch);
  const char* actionLabels[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL)};
  TouchActionButtons::draw(renderer, actions, actionLabels, selectedOption, selectedOption, UI_10_FONT_ID);
}

void drawFailure(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, const int top,
                 const char* message) {
  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
  const int messageWidth = screen.width - metrics.contentSidePadding * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, message, messageWidth, 3);
  int messageY = top + 40;
  for (const auto& line : messageLines) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
    messageY += lineHeight + 4;
  }
}

}  // namespace BookOrbitSyncScreens
