#include "HeaderDate.h"

#include <GfxRenderer.h>
#include <HalClock.h>

#include <algorithm>
#include <cstddef>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr int kHeaderDateRightInset = 12;
constexpr int kHeaderDateBottomGap = 10;

char dateSeparatorChar() {
  switch (SETTINGS.dateSeparator) {
    case CrossPointSettings::DATE_SEPARATOR_PERIOD:
      return '.';
    case CrossPointSettings::DATE_SEPARATOR_HYPHEN:
      return '-';
    case CrossPointSettings::DATE_SEPARATOR_SLASH:
    default:
      return '/';
  }
}

bool formatHeaderDateImpl(char* buf, const size_t len) {
  if (!halClock.isAvailable()) return false;
  if (!SETTINGS.clockDateHasBeenSynced) return false;
#if defined(SIMULATOR) && !defined(CROSSPOINT_SIMULATOR_HAS_DATE_FORMAT)
  // Keep compatibility with older downloaded simulator libraries.
  return halClock.formatDate(buf, len, SETTINGS.clockUtcOffsetQ);
#elif defined(SIMULATOR) && !defined(CROSSPOINT_SIMULATOR_HAS_DATE_SEPARATOR)
  // Older simulator libraries support date formats but always emit slashes.
  if (!halClock.formatDate(buf, len, SETTINGS.clockUtcOffsetQ,
                           static_cast<HalClock::DateFormat>(SETTINGS.dateFormat))) {
    return false;
  }
  const char separator = dateSeparatorChar();
  if (separator != '/') {
    for (char* p = buf; *p != '\0'; ++p) {
      if (*p == '/') *p = separator;
    }
  }
  return true;
#else
  return halClock.formatDate(buf, len, SETTINGS.clockUtcOffsetQ, static_cast<HalClock::DateFormat>(SETTINGS.dateFormat),
                             dateSeparatorChar());
#endif
}
}  // namespace

bool formatHeaderDateText(char* buffer, const size_t length) { return formatHeaderDateImpl(buffer, length); }

int headerDateReservedWidth(const GfxRenderer& renderer) {
  char dateBuf[13];
  if (!formatHeaderDateImpl(dateBuf, sizeof(dateBuf))) return 0;

  return renderer.getTextWidth(UI_10_FONT_ID, dateBuf) + kHeaderDateRightInset;
}

int headerDateLineBottomY(const GfxRenderer&, const ThemeMetrics& metrics, const int headerHeight) {
  const int effectiveHeaderHeight = headerHeight >= 0 ? headerHeight : metrics.headerHeight;
  return metrics.topPadding + effectiveHeaderHeight - kHeaderDateBottomGap;
}

void drawHeaderDate(const GfxRenderer& renderer, const int pageWidth, const ThemeMetrics& metrics,
                    const int headerHeight) {
  drawHeaderDateAtLineBottom(renderer, pageWidth, headerDateLineBottomY(renderer, metrics, headerHeight));
}

void drawHeaderDateAtLineBottom(const GfxRenderer& renderer, const int pageWidth, const int lineBottomY) {
  constexpr int dateFontId = UI_10_FONT_ID;
  drawHeaderDateAtBaseline(renderer, pageWidth,
                           lineBottomY - renderer.getLineHeight(dateFontId) + renderer.getFontAscenderSize(dateFontId));
}

void drawHeaderDateAtBaseline(const GfxRenderer& renderer, const int pageWidth, const int baselineY) {
  char dateBuf[13];
  if (!formatHeaderDateImpl(dateBuf, sizeof(dateBuf))) return;

  constexpr int dateFontId = UI_10_FONT_ID;
  const int textWidth = renderer.getTextWidth(dateFontId, dateBuf);
  const int dateX = pageWidth - kHeaderDateRightInset - textWidth;
  const int dateY = baselineY - renderer.getFontAscenderSize(dateFontId);
  renderer.drawText(dateFontId, std::max(0, dateX), std::max(0, dateY), dateBuf);
}
