#pragma once
#ifndef SIMULATOR
#include <BoardConfig.h>
#endif
#include <FreeInkUIGfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "AppCapabilities.h"
#include "CrossPointSettings.h"
#include "UITheme.h"

namespace UiThemeTokensDetail {
inline int16_t scaledListMetric(const int metric) {
  constexpr int baseFontSize = 10;
  int scaleFontSize = baseFontSize;
  switch (SETTINGS.uiScale) {
    case CrossPointSettings::UI_SCALE_LARGE:
      scaleFontSize = 12;
      break;
    case CrossPointSettings::UI_SCALE_SMALL:
    default:
      break;
  }
  return static_cast<int16_t>((metric * scaleFontSize + baseFontSize / 2) / baseFontSize);
}
}  // namespace UiThemeTokensDetail

enum class UiListRowType : uint8_t {
  SingleLine,
  WithSubtitle,
};

inline int16_t uiListRowHeight(const freeink::ui::ThemeTokens& tokens, const UiListRowType rowType) {
  if (gpio.hasTouch()) return tokens.rowHeight;

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  switch (rowType) {
    case UiListRowType::SingleLine:
      return UiThemeTokensDetail::scaledListMetric(metrics.listRowHeight);
    case UiListRowType::WithSubtitle:
      return UiThemeTokensDetail::scaledListMetric(metrics.listWithSubtitleRowHeight);
  }
  return tokens.rowHeight;
}

inline uint16_t configureUiList(freeink::ui::ListProps& props, const freeink::ui::ThemeTokens& tokens,
                                const freeink::ui::Rect rect, const UiListRowType rowType = UiListRowType::SingleLine) {
  // Button-only menus used compact, single-line rows before their FreeInkUI
  // migration. Keep that layout unless the caller explicitly needs a subtitle.
#if CROSSINK_APP_CAP_TOUCH
  if (!gpio.hasTouch() && rowType == UiListRowType::SingleLine) props.labelText.maxLines = 1;
#else
  if (rowType == UiListRowType::SingleLine) props.labelText.maxLines = 1;
#endif
  if (props.rowHeight <= 0) props.rowHeight = uiListRowHeight(tokens, rowType);
  // Subtitle rows may wrap their labels, so retain FreeInkUI's font-derived
  // two-line height and avoid overlapping the following item.
  if (props.labelText.maxLines > 1) props.rowHeight = std::max(props.rowHeight, tokens.rowHeight);
  if (props.rowGap < 0) props.rowGap = tokens.listRowGap;
  return freeink::ui::listVisibleRows(rect, props.rowHeight, props.rowGap);
}

inline void configureUiListSectionHeaders(freeink::ui::ListProps& props, const freeink::ui::ThemeTokens& tokens) {
  if (SETTINGS.uiTheme != CrossPointSettings::UI_THEME::ROUNDEDRAFF) return;

  props.headerText = tokens.titleText;
  props.headerText.bold = true;
  props.sectionGap = halTiltSensor.isAvailable() ? 10 : 20;
}

inline const char* uiListSectionHeaderLabel(std::string& storage, const char* label) {
  if (SETTINGS.uiTheme != CrossPointSettings::UI_THEME::ROUNDEDRAFF) return label;

  storage = label != nullptr ? label : "";
  std::transform(storage.begin(), storage.end(), storage.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return storage.c_str();
}

// Merges the active UITheme's shape with uiScale-derived sizes. Touch builds
// retain FreeInkUI's larger font-derived rows; button devices use the theme's
// compact one-line/two-line metrics, scaled with the selected UI font size.
inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
  tokens.listRowGap = static_cast<int16_t>(metrics.listRowGap);
  tokens.listRowRadius = static_cast<uint8_t>(metrics.listRowRadius);
  tokens.listInset = static_cast<int16_t>(metrics.listInset);
  tokens.listSidePadding = static_cast<int16_t>(metrics.listSidePadding);
  tokens.listSelectionStyle = static_cast<fui::SelectionStyle>(metrics.listSelectionStyle);
  tokens.listScrollWidth = static_cast<int16_t>(metrics.listScrollWidth);
  tokens.listScrollSide = static_cast<uint8_t>(metrics.listScrollSide);
  // The X4 Pro panel sits recessed behind the bezel, so an edge-hugging scroll
  // indicator disappears under it. Push it inward far enough to clear the bezel.
#ifndef SIMULATOR
  tokens.listScrollInset = (BoardConfig::isX4Pro() || CROSSINK_APP_DEVICE_X4CLASSIC) ? 7 : 0;
#endif
  tokens.headerSidePadding = static_cast<int16_t>(metrics.headerSidePadding);
  tokens.headerUnderline = static_cast<uint8_t>(metrics.headerUnderlineSize);
  tokens.headerTitleAlign = static_cast<fui::TextAlign>(metrics.headerTitleAlign);
  tokens.bodyText.bold = metrics.listTitleBold;
  return tokens;
}
