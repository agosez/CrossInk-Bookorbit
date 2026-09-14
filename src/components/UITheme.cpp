#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace {
constexpr char kWidthPlaceholder[] = "[WIDTH]";
constexpr char kHeightPlaceholder[] = "[HEIGHT]";
constexpr size_t kWidthPlaceholderLength = sizeof(kWidthPlaceholder) - 1;
constexpr size_t kHeightPlaceholderLength = sizeof(kHeightPlaceholder) - 1;

int drawCenteredTextLines(const GfxRenderer& renderer, const Rect screen, const int fontId, int y,
                          const std::vector<std::string>& lines, const bool black, const EpdFontFamily::Style style,
                          const int lineSpacing) {
  if (lines.empty()) return 0;

  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    UITheme::drawCenteredText(renderer, screen, fontId, y, line.c_str(), black, style);
    y += lineHeight + lineSpacing;
  }
  return lineHeight * static_cast<int>(lines.size()) + lineSpacing * (static_cast<int>(lines.size()) - 1);
}

std::string addBmpSuffix(const std::string& path, const char* suffix) {
  const size_t extPos = path.rfind(".bmp");
  if (extPos == std::string::npos) {
    return path + suffix;
  }
  std::string suffixedPath = path;
  suffixedPath.insert(extPos, suffix);
  return suffixedPath;
}
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() : currentMetrics(&LyraMetrics::values), currentTheme(std::make_unique<LyraTheme>()) {
  // Static construction must not log or depend on cross-TU serial initialization;
  // main.cpp reloads the saved theme after setup.
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
      LOG_DBG("UI", "Using Lyra Carousel theme");
      currentTheme = std::make_unique<LyraCarouselTheme>();
      currentMetrics = &LyraCarouselMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::MINIMAL:
      LOG_DBG("UI", "Using Minimal theme");
      currentTheme = std::make_unique<MinimalTheme>();
      currentMetrics = &MinimalMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::DASHBOARD:
      LOG_DBG("UI", "Using Dashboard theme");
      currentTheme = std::make_unique<DashboardTheme>();
      currentMetrics = &DashboardMetrics::values;
      break;
    default:
      LOG_ERR("UI", "Unknown theme %d, falling back to Classic", static_cast<int>(type));
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
  }
  metricsValid = false;
}

const ThemeMetrics& UITheme::getMetrics() const {
#if CROSSINK_APP_CAP_TOUCH
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  if (!metricsValid || touch != metricsForTouch) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    metricsForTouch = touch;
    metricsValid = true;
  }
#else
  if (!metricsValid) {
    adjustedMetrics = *currentMetrics;
    metricsValid = true;
  }
#endif
  return adjustedMetrics;
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  return UITheme::getInstance().getTheme().getListPageItems(availableHeight, hasSubtitle);
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  (void)hasSideButtonHints;
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += currentMetrics->buttonHintsHeight;
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += currentMetrics->buttonHintsHeight;
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int coverHeight) {
  if (coverHeight <= 0) {
    return "";
  }
  // Use int64_t so large heights cannot overflow before division.
  const int coverWidth = static_cast<int>((static_cast<int64_t>(coverHeight) * 2 + 1) / 3);
  return getCoverThumbPath(coverBmpPath, coverWidth, coverHeight);
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int width, int height) {
  if (width <= 0 || height <= 0) {
    return "";
  }
  const size_t initialWidthPos = coverBmpPath.find(kWidthPlaceholder, 0);
  const size_t initialHeightPos = coverBmpPath.find(kHeightPlaceholder, 0);
  const bool hasWidthPlaceholder = initialWidthPos != std::string::npos;
  const bool hasHeightPlaceholder = initialHeightPos != std::string::npos;

  if (!hasWidthPlaceholder && !hasHeightPlaceholder) {
    return coverBmpPath;
  }
  if ((hasWidthPlaceholder &&
       coverBmpPath.find(kWidthPlaceholder, initialWidthPos + kWidthPlaceholderLength) != std::string::npos) ||
      (hasHeightPlaceholder &&
       coverBmpPath.find(kHeightPlaceholder, initialHeightPos + kHeightPlaceholderLength) != std::string::npos)) {
    return "";
  }
  if (!hasHeightPlaceholder) {
    return "";
  }

  std::string thumbPath = coverBmpPath;
  size_t widthPos = thumbPath.find(kWidthPlaceholder, 0);
  if (widthPos != std::string::npos) {
    thumbPath.replace(widthPos, kWidthPlaceholderLength, std::to_string(width));
  }
  size_t pos = thumbPath.find(kHeightPlaceholder, 0);
  if (pos != std::string::npos) {
    if (hasWidthPlaceholder) {
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
    } else {
      std::string legacyPath = thumbPath;
      legacyPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(width) + "x" + std::to_string(height));
      if (!Storage.exists(thumbPath.c_str()) && Storage.exists(legacyPath.c_str())) {
        return legacyPath;
      }
    }
  }
  return thumbPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const auto statusBar = SETTINGS.statusBarSpec();
  // Reserve the clock lane independently of the current board so orientation
  // and layout do not change when the same settings are used on another device.
  return (statusBar.textLaneVisible(true) ? metrics.statusBarVerticalMargin : 0) +
         (statusBar.showsProgressBar() ? statusBar.progressBarHeightPx + metrics.progressBarMarginTop : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const auto statusBar = SETTINGS.statusBarSpec();
  return statusBar.showsProgressBar() ? statusBar.progressBarHeightPx + metrics.progressBarMarginTop : 0;
}

int UITheme::getTopStatusBarInset(const GfxRenderer& renderer) {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  // The Sticky panel remains usable closer to its top edge than the shared
  // status-bar layout assumes. Keep the clock and battery in that space.
  (void)renderer;
  return -5;
#elif (defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO) || \
    (defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC)
  // The X4 Pro and X4 Classic panels sit slightly recessed behind the
  // portrait top bezel.
  return renderer.getOrientation() == GfxRenderer::Orientation::Portrait ? 5 : 0;
#endif

  return 0;
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

int UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, const Rect screen, const int fontId, int y,
                                     const char* text, const int maxLines, const bool black,
                                     const EpdFontFamily::Style style, const int lineSpacing) {
  const auto lines = renderer.wrappedText(fontId, text, screen.width, maxLines, style);
  return drawCenteredTextLines(renderer, screen, fontId, y, lines, black, style, lineSpacing);
}

int UITheme::drawCenteredWrappedTextAtCenter(const GfxRenderer& renderer, const Rect screen, const int fontId,
                                             const int y, const char* text, const int maxLines, const bool black,
                                             const EpdFontFamily::Style style, const int lineSpacing) {
  const auto lines = renderer.wrappedText(fontId, text, screen.width, maxLines, style);
  const int lineStep = renderer.getLineHeight(fontId) + lineSpacing;
  const int top = y - std::max(0, static_cast<int>(lines.size()) - 1) * lineStep / 2;
  return drawCenteredTextLines(renderer, screen, fontId, top, lines, black, style, lineSpacing);
}
