#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "AppVersion.h"
#include "BackupStatsActivity.h"
#include "BookOrbitSettingsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/DictionaryRegistry.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_TAB = 2;
}  // namespace

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

namespace {
constexpr int systemVersionFooterSideMargin = 20;
constexpr int systemVersionFooterBottomInset = 15;
constexpr size_t controlsParentBaseCount = 3;
constexpr size_t controlsPowerMinCount = 2;
constexpr size_t controlsPowerMaxCount = 3;
constexpr size_t controlsFrontButtonCount = 6;
constexpr size_t controlsSideButtonCount = 3;
constexpr int touchSettingsRowHeightScale = 2;
constexpr int touchSettingsTabBarHeightScale = 2;

int settingsTabBarTop(const ThemeMetrics& metrics) { return CompactHeader::headerBottomY(metrics); }

int settingsRowHeightScale(const bool hasTouch) { return hasTouch ? touchSettingsRowHeightScale : 1; }

int settingsTabBarHeight(const ThemeMetrics& metrics, const bool hasTouch) {
  return metrics.tabBarHeight * (hasTouch ? touchSettingsTabBarHeightScale : 1);
}

int settingsSubmenuHeaderOffset(const GfxRenderer& renderer, const ThemeMetrics& metrics, const bool hasSubmenuTitle) {
  if (!hasSubmenuTitle) return 0;
  return renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
}

Rect settingsListRect(const ThemeMetrics& metrics, const int pageWidth, const int pageHeight, const bool hasTouch) {
  const int tabBarTop = settingsTabBarTop(metrics);
  const int listTop = tabBarTop + settingsTabBarHeight(metrics, hasTouch) + metrics.verticalSpacing;
  return Rect{0, listTop, pageWidth, pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing};
}

Rect settingsHeaderRect(const ThemeMetrics& metrics, const int pageWidth) {
  return Rect{0, metrics.topPadding, pageWidth, CompactHeader::headerBottomY(metrics) - metrics.topPadding};
}

Rect settingsRowsRect(const GfxRenderer& renderer, const ThemeMetrics& metrics, const int pageWidth,
                      const int pageHeight, const bool hasTouch, const bool hasSubmenuTitle) {
  Rect rect = settingsListRect(metrics, pageWidth, pageHeight, hasTouch);
  const int headerOffset = settingsSubmenuHeaderOffset(renderer, metrics, hasSubmenuTitle);
  rect.y += headerOffset;
  rect.height = std::max(0, rect.height - headerOffset);
  return rect;
}

uint8_t enumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

void drawCenteredTextLine(const GfxRenderer& renderer, const int pageWidth, const int y, const std::string& text) {
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, text.c_str());
  const int labelX = (pageWidth - labelWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, labelX, y, text.c_str());
}

bool isVersionBreakChar(const char c) { return c == ' ' || c == '-' || c == '+' || c == '.' || c == '_'; }

std::string formatUtcOffset(uint8_t biasedQ) {
  if (biasedQ > 104) biasedQ = 48;
  const int totalMinutes = (static_cast<int>(biasedQ) - 48) * 15;
  const bool neg = totalMinutes < 0;
  const int absMinutes = neg ? -totalMinutes : totalMinutes;
  char buf[16];
  snprintf(buf, sizeof(buf), "UTC%c%d:%02d", neg ? '-' : '+', absMinutes / 60, absMinutes % 60);
  return buf;
}

std::string formatCompactDuration(const uint32_t seconds) {
  char buf[24];
  if (seconds < 60) {
    snprintf(buf, sizeof(buf), "%lus", static_cast<unsigned long>(seconds));
  } else if (seconds % 60 == 0) {
    snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(buf, sizeof(buf), "%lum %lus", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  }
  return buf;
}

void drawSystemVersionFooter(const GfxRenderer& renderer, const int pageWidth, const int pageHeight,
                             const ThemeMetrics& metrics) {
  const std::string label = "CrossInk " CROSSINK_VERSION;
  const int maxWidth = pageWidth - systemVersionFooterSideMargin * 2;
  const int bottomLineY =
      pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - systemVersionFooterBottomInset;

  if (renderer.getTextWidth(SMALL_FONT_ID, label.c_str()) <= maxWidth) {
    drawCenteredTextLine(renderer, pageWidth, bottomLineY, label);
    return;
  }

  size_t fallbackBreak = std::string::npos;
  size_t preferredBreak = std::string::npos;
  for (size_t i = 1; i < label.size(); i++) {
    if (!isVersionBreakChar(label[i - 1])) continue;

    const std::string firstLine = label.substr(0, i);
    if (renderer.getTextWidth(SMALL_FONT_ID, firstLine.c_str()) > maxWidth) break;

    fallbackBreak = i;
    const std::string secondLine = label.substr(i);
    if (renderer.getTextWidth(SMALL_FONT_ID, secondLine.c_str()) <= maxWidth) {
      preferredBreak = i;
    }
  }

  const size_t lineBreak = preferredBreak != std::string::npos ? preferredBreak : fallbackBreak;
  const std::string firstLine = lineBreak == std::string::npos
                                    ? renderer.truncatedText(SMALL_FONT_ID, label.c_str(), maxWidth)
                                    : label.substr(0, lineBreak);
  const std::string secondLine = lineBreak == std::string::npos
                                     ? ""
                                     : renderer.truncatedText(SMALL_FONT_ID, label.substr(lineBreak).c_str(), maxWidth);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  drawCenteredTextLine(renderer, pageWidth, bottomLineY - lineHeight, firstLine);
  drawCenteredTextLine(renderer, pageWidth, bottomLineY, secondLine);
}

std::string formatSettingValue(const SettingInfo& setting) {
  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
      return tr(STR_SLEEP_NEVER);
    }
    char valueBuffer[32];
    snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
             static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
    return valueBuffer;
  }
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    return std::to_string(SETTINGS.*(setting.valuePtr)) + "%";
  }
  if (setting.valuePtr == &CrossPointSettings::readingIdleTimeThresholdUnits) {
    return formatCompactDuration(SETTINGS.getReadingIdleTimeThresholdSeconds());
  }
  if (setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ) {
    return formatUtcOffset(SETTINGS.*(setting.valuePtr));
  }
  return std::to_string(SETTINGS.*(setting.valuePtr));
}

uint8_t valueDisplayIndexForRawValue(const SettingInfo& setting, const uint8_t rawValue) {
  const uint8_t min = setting.valueRange.min;
  const uint8_t max = setting.valueRange.max;
  const uint8_t step = setting.valueRange.step == 0 ? 1 : setting.valueRange.step;
  const uint8_t clampedValue = std::clamp(rawValue, min, max);
  const uint8_t offset = clampedValue > min ? clampedValue - min : 0;
  return static_cast<uint8_t>((offset + step / 2) / step);
}

uint8_t rawValueForValueDisplayIndex(const SettingInfo& setting, const uint8_t displayIndex) {
  const uint8_t step = setting.valueRange.step == 0 ? 1 : setting.valueRange.step;
  const uint16_t rawValue = static_cast<uint16_t>(setting.valueRange.min) + static_cast<uint16_t>(displayIndex) * step;
  return static_cast<uint8_t>(std::min<uint16_t>(rawValue, setting.valueRange.max));
}

uint8_t valueOptionCount(const SettingInfo& setting) {
  const uint8_t step = setting.valueRange.step == 0 ? 1 : setting.valueRange.step;
  return static_cast<uint8_t>(((setting.valueRange.max - setting.valueRange.min) / step) + 1);
}

std::string trimAsciiSpaces(const std::string& value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}
}  // namespace

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool dismissOnUpSwipe)
    : Activity("Settings", renderer, mappedInput),
      dismissOnUpSwipe(dismissOnUpSwipe),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  displaySleepSettings.clear();
  readerSettings.clear();
  readerFontSettings.clear();
  readerPageLayoutSettings.clear();
  controlsSettings.clear();
  controlsPowerSettings.clear();
  controlsFrontButtonSettings.clear();
  controlsSideButtonSettings.clear();
  systemSettings.clear();
  systemDeviceSettings.clear();
  systemFilesCacheSettings.clear();
  systemReadingStatsSettings.clear();
  systemGlobalStatsSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  dictionaryRegistry.refreshIfDirty();
  const auto allSettings = getSettingsList(&sdFontSystem.registry(), &dictionaryRegistry);
  displaySettings = buildGroupedDisplaySettingsList(allSettings);
#ifndef SIMULATOR
  if (BoardConfig::isX4Pro()) {
    displaySettings.erase(
        std::remove_if(displaySettings.begin(), displaySettings.end(),
                       [](const SettingInfo& setting) { return setting.valuePtr == &CrossPointSettings::fadingFix; }),
        displaySettings.end());
  }
#endif
  displaySleepSettings = buildDisplaySleepSettingsList(allSettings);
  readerSettings = buildReaderSettingsParentList(allSettings);
  readerFontSettings = buildReaderFontSettingsList(allSettings);
  readerPageLayoutSettings = buildReaderPageLayoutSettingsList(allSettings);
  systemSettings = buildSystemSettingsParentList(allSettings);
  systemDeviceSettings = buildSystemDeviceSettingsList(allSettings);
  systemFilesCacheSettings = buildSystemFilesCacheSettingsList(allSettings);
  systemReadingStatsSettings = buildSystemReadingStatsSettingsList(allSettings);
  systemGlobalStatsSettings = buildSystemGlobalStatsSettingsList(allSettings);
  controlsSettings = buildControlsSettingsParentList(allSettings);
  controlsPowerSettings = buildControlsPowerSettingsList(allSettings);
#if CROSSINK_APP_CAP_TOUCH
  if (!gpio.hasTouch()) {
    controlsFrontButtonSettings = buildControlsFrontButtonSettingsList(allSettings);
  }
  controlsSideButtonSettings = buildControlsSideButtonSettingsList(allSettings);

  const bool hasTouch = gpio.hasTouch();
  const size_t expectedControlsCount = controlsParentBaseCount - (hasTouch ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN) ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION) ? 1u : 0u);
  const size_t expectedFrontButtonCount = hasTouch ? 0u : controlsFrontButtonCount;
#else
  controlsFrontButtonSettings = buildControlsFrontButtonSettingsList(allSettings);
  controlsSideButtonSettings = buildControlsSideButtonSettingsList(allSettings);

  const size_t expectedControlsCount = controlsParentBaseCount +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN) ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION) ? 1u : 0u);
  constexpr size_t expectedFrontButtonCount = controlsFrontButtonCount;
#endif
  if (controlsSettings.size() != expectedControlsCount || controlsPowerSettings.size() < controlsPowerMinCount ||
      controlsPowerSettings.size() > controlsPowerMaxCount ||
      controlsFrontButtonSettings.size() != expectedFrontButtonCount ||
      controlsSideButtonSettings.size() != controlsSideButtonCount) {
    LOG_ERR("SET", "Unexpected controls menu counts: controls=%u/%u power=%u front=%u side=%u",
            static_cast<uint32_t>(controlsSettings.size()), static_cast<uint32_t>(expectedControlsCount),
            static_cast<uint32_t>(controlsPowerSettings.size()),
            static_cast<uint32_t>(controlsFrontButtonSettings.size()),
            static_cast<uint32_t>(controlsSideButtonSettings.size()));
  }

  setCurrentSettingsForCategory();
}

void SettingsActivity::setCurrentSettingsForCategory() {
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = activeSubmenu == SettingAction::DisplaySleepScreen ? &displaySleepSettings : &displaySettings;
      break;
    case 1:
      switch (activeSubmenu) {
        case SettingAction::ReaderFontOptions:
          currentSettings = &readerFontSettings;
          break;
        case SettingAction::ReaderPageLayout:
          currentSettings = &readerPageLayoutSettings;
          break;
        default:
          currentSettings = &readerSettings;
          break;
      }
      break;
    case 2:
      switch (activeSubmenu) {
        case SettingAction::ControlsPowerButton:
          currentSettings = &controlsPowerSettings;
          break;
        case SettingAction::ControlsFrontButtons:
          currentSettings = &controlsFrontButtonSettings;
          break;
        case SettingAction::ControlsSideButtons:
          currentSettings = &controlsSideButtonSettings;
          break;
        default:
          currentSettings = &controlsSettings;
          break;
      }
      break;
    case 3:
      switch (activeSubmenu) {
        case SettingAction::SystemDevice:
          currentSettings = &systemDeviceSettings;
          break;
        case SettingAction::SystemFilesCache:
          currentSettings = &systemFilesCacheSettings;
          break;
        case SettingAction::SystemReadingStats:
          currentSettings = &systemReadingStatsSettings;
          break;
        case SettingAction::SystemGlobalStats:
          currentSettings = &systemGlobalStatsSettings;
          break;
        default:
          currentSettings = &systemSettings;
          break;
      }
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::enterCategory(int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  activeSubmenu = SettingAction::None;
  parentSubmenu = SettingAction::None;
  setCurrentSettingsForCategory();
  showSettingSelection = true;
}

StrId SettingsActivity::activeSubmenuTitleId() const {
  switch (activeSubmenu) {
    case SettingAction::DisplaySleepScreen:
      return StrId::STR_DISPLAY_SLEEP_SCREEN;
    case SettingAction::ReaderFontOptions:
      return StrId::STR_READER_FONT_OPTIONS;
    case SettingAction::ReaderPageLayout:
      return StrId::STR_READER_PAGE_LAYOUT;
    case SettingAction::ControlsPowerButton:
      return StrId::STR_POWER_BUTTON;
    case SettingAction::ControlsFrontButtons:
      return StrId::STR_FRONT_BUTTONS;
    case SettingAction::ControlsSideButtons:
      return StrId::STR_SIDE_BUTTONS;
    case SettingAction::SystemDevice:
      return StrId::STR_SYSTEM_DEVICE;
    case SettingAction::SystemFilesCache:
      return StrId::STR_SYSTEM_FILES_CACHE;
    case SettingAction::SystemReadingStats:
      return StrId::STR_READING_STATS;
    case SettingAction::SystemGlobalStats:
      return StrId::STR_ALL_TIME_STATS;
    default:
      return StrId::STR_NONE_OPT;
  }
}

void SettingsActivity::openSubmenu(SettingAction action) {
  parentSubmenu = activeSubmenu;
  activeSubmenu = action;
  setCurrentSettingsForCategory();
  selectedSettingIndex = 1;
  showSettingSelection = true;
  while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
         (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
  }
}

void SettingsActivity::closeSubmenu() {
  activeSubmenu = parentSubmenu;
  parentSubmenu = SettingAction::None;
  setCurrentSettingsForCategory();
  selectedSettingIndex = 1;
  showSettingSelection = true;
}

bool SettingsActivity::currentSettingUsesOptionMenu(const SettingInfo& setting) const {
  return setting.nameId != StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM &&
         settingEnumOptionCount(setting) > 2 &&
         (setting.valuePtr != nullptr || (setting.valueGetter && setting.valueSetter));
}

void SettingsActivity::openEnumOptionPicker(const SettingInfo& setting) {
  const size_t optionCount = settingEnumOptionCount(setting);
  if (optionCount == 0) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (uint8_t i = 0; i < optionCount; i++) {
    options.push_back(settingEnumOptionLabel(setting, i));
  }

  uint8_t currentIndex = 0;
  if (setting.valuePtr != nullptr) {
    currentIndex = enumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
  } else if (setting.valueGetter) {
    currentIndex = setting.valueGetter();
  }
  if (currentIndex >= optionCount) currentIndex = 0;

  const SettingInfo selectedSetting = setting;
  optionPopup.show(setting.nameId, options, currentIndex, [this, selectedSetting](int selectedIndex) {
    if (selectedSetting.valuePtr != nullptr) {
      SETTINGS.*(selectedSetting.valuePtr) =
          enumRawValueForDisplayIndex(selectedSetting, static_cast<uint8_t>(selectedIndex));
    } else if (selectedSetting.valueSetter) {
      selectedSetting.valueSetter(static_cast<uint8_t>(selectedIndex));
    }

    const bool sleepScreenChanged = selectedSetting.valuePtr == &CrossPointSettings::sleepScreen;
    const bool quickResumeTimeoutChanged = selectedSetting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;
    syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
    SETTINGS.saveToFile();
    rebuildSettingsLists();
    requestUpdate();
    maybePromptKeepClockInSleep(selectedSetting);
  });
  requestUpdate();
}

void SettingsActivity::maybePromptKeepClockInSleep(const SettingInfo& changed) {
  if (changed.valuePtr != &CrossPointSettings::hideClock) return;
  // Only worth asking once the clock is actually visible somewhere, on a board with no
  // RTC to carry the time across sleep, and only while the option is still off.
  if (!SETTINGS.shouldShowClockInReader() && !SETTINGS.shouldShowClockOutsideReader()) return;
  if (halClock.isAvailable() || SETTINGS.keepClockInSleep != 0) return;

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_CLOCK_SLEEP_PROMPT_TITLE),
                                                                tr(STR_CLOCK_SLEEP_PROMPT_BODY)),
                         [this](const ActivityResult& promptResult) {
                           if (promptResult.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           SETTINGS.keepClockInSleep = 1;
                           SETTINGS.saveToFile();
                           rebuildSettingsLists();
                           // Point at the two settings that make the displayed time actually correct.
                           startActivityForResult(std::make_unique<ConfirmationActivity>(
                                                      renderer, mappedInput, tr(STR_CLOCK_SLEEP_ENABLED_TITLE),
                                                      tr(STR_CLOCK_SLEEP_ENABLED_BODY)),
                                                  [this](const ActivityResult&) { requestUpdate(); });
                         });
}

void SettingsActivity::openScreenMarginPicker(const SettingInfo& setting) {
  const uint8_t optionCount = valueOptionCount(setting);
  if (optionCount == 0 || setting.valuePtr == nullptr) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (uint8_t i = 0; i < optionCount; i++) {
    options.push_back(std::to_string(rawValueForValueDisplayIndex(setting, i)));
  }

  uint8_t currentIndex = valueDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
  if (currentIndex >= optionCount) currentIndex = 0;

  const SettingInfo selectedSetting = setting;
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "SettingsValueSelect", selectedSetting.nameId,
                                                std::move(options), currentIndex, false, true),
      [this, selectedSetting](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
        if (selection != nullptr && selectedSetting.valuePtr != nullptr) {
          SETTINGS.*(selectedSetting.valuePtr) = rawValueForValueDisplayIndex(selectedSetting, selection->index);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openLanguagePicker() {
  const int languageCount = static_cast<int>(getLanguageCount());

  std::vector<std::string> options;
  options.reserve(languageCount);
  for (int i = 0; i < languageCount; i++) {
    options.push_back(I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[i])));
  }

  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  int currentIndex = (it != end) ? static_cast<int>(std::distance(begin, it)) : 0;

  optionPopup.show(StrId::STR_LANGUAGE, options, currentIndex, [this](int selectedIndex) {
    const int languageCount = static_cast<int>(getLanguageCount());
    if (selectedIndex < 0 || selectedIndex >= languageCount) {
      requestUpdate();
      return;
    }

    const uint8_t langIndex = SORTED_LANGUAGE_INDICES[selectedIndex];
    {
      RenderLock lock(*this);
      I18N.setLanguage(static_cast<Language>(langIndex));
    }

    SETTINGS.language = langIndex;
    SETTINGS.saveToFile();
    requestUpdate();
  });
  requestUpdate();
}

void SettingsActivity::openStringEditor(const SettingInfo& setting) {
  std::string initialText;
  if (setting.stringGetter) {
    initialText = setting.stringGetter();
  } else if (setting.stringMaxLen > 0) {
    initialText = reinterpret_cast<const char*>(&SETTINGS) + setting.stringOffset;
  }
  if (setting.nameId == StrId::STR_DEVICE_NAME && initialText.empty()) {
    initialText = SETTINGS.getEffectiveDeviceName();
  }

  const size_t maxLength = setting.stringMaxLen > 0 ? setting.stringMaxLen - 1 : 0;
  const size_t minLength = setting.nameId == StrId::STR_DEVICE_NAME ? CrossPointSettings::MIN_DEVICE_NAME_LENGTH : 0;
  const SettingInfo selectedSetting = setting;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(setting.nameId),
                                                                 initialText, maxLength, InputType::Text, minLength),
                         [this, selectedSetting](const ActivityResult& result) {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }

                           const auto* kb = std::get_if<KeyboardResult>(&result.data);
                           if (kb == nullptr) {
                             requestUpdate();
                             return;
                           }

                           const std::string value = trimAsciiSpaces(kb->text);
                           if (selectedSetting.nameId == StrId::STR_DEVICE_NAME &&
                               (value.length() < CrossPointSettings::MIN_DEVICE_NAME_LENGTH ||
                                value.length() > CrossPointSettings::MAX_DEVICE_NAME_LENGTH)) {
                             requestUpdate();
                             return;
                           }

                           if (selectedSetting.stringSetter) {
                             selectedSetting.stringSetter(value);
                           } else if (selectedSetting.stringMaxLen > 0) {
                             char* ptr = reinterpret_cast<char*>(&SETTINGS) + selectedSetting.stringOffset;
                             strncpy(ptr, value.c_str(), selectedSetting.stringMaxLen - 1);
                             ptr[selectedSetting.stringMaxLen - 1] = '\0';
                           }
                           SETTINGS.saveToFile();
                           requestUpdate();
                         });
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Dictionary names and paths are needed only while settings are open. Keep
  // the catalog out of the reader's steady-state heap.
  dictionaryRegistry.discover();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  activeSubmenu = SettingAction::None;
  parentSubmenu = SettingAction::None;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &SettingsActivity::onRowEvent, this);
  app.on(ACTION_TAB, &SettingsActivity::onTabEvent, this);
  app.setScreen(&SettingsActivity::settingsScreen, this);

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  topIndex = 0;
}

void SettingsActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<SettingsActivity*>(user);
  if (self->optionPopup.isActive()) return;
  if (event.value < 0 || event.value >= categoryCount) return;
  self->selectedSettingIndex = 0;
  self->enterCategory(event.value);
  self->topIndex = 0;
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  self->app.clearTapFlash();
}

void SettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<SettingsActivity*>(user);
  if (self->optionPopup.isActive()) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->settingsCount)) return;
  if ((*self->currentSettings)[event.value].type == SettingType::SECTION_HEADER) return;
  self->selectedSettingIndex = event.value + 1;
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  self->app.clearTapFlash();
  self->toggleCurrentSetting();
}

void SettingsActivity::onExit() {
  dictionaryRegistry.clear();
  sdFontSystem.releaseRegistry();
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme and UI-scale changes take effect immediately, on this screen —
  // reload the theme and re-derive the app's fonts and tokens so the very
  // next repaint is in the new look.
  if (valuePtr == &CrossPointSettings::uiTheme) {
    UITheme::getInstance().reload();
  } else if (valuePtr != &CrossPointSettings::uiScale) {
    return;
  }
  const auto spec = uiScaleSpec();
  uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, spec.smallFontId);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, spec.bodyFontId);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, spec.titleFontId);
  app.setTheme(uiThemeTokens(uiTarget));
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (TouchHeaderBackButton::wasTapped(mappedInput, settingsHeaderRect(metrics, renderer.getScreenWidth()))) {
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showSettingSelection = true;
    if (selectedSettingIndex == 0) {
      enterCategory((selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0);
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
      return;
    }
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      showSettingSelection = true;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the tab and row
  // hit rects; route the snapshot and let the handlers dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint here: it would cost a second e-ink refresh
      // per tap and paint a transient double pill on tab switches whose
      // erased black leaves a partial-refresh ghost.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onTabEvent/onRowEvent
    }
  }

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (dismissOnUpSwipe && swipe == MappedInputManager::SwipeDir::Up) {
    SETTINGS.saveToFile();
    finish();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, settingsCount);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  // Buttons walk the tab band (index 0) plus the rows (1..settingsCount).
  const auto moveSelection = [this](int index, const bool forward) {
    while (index > 0 && index <= settingsCount && (*currentSettings)[index - 1].type == SettingType::SECTION_HEADER) {
      index = forward ? ButtonNavigator::nextIndex(index, settingsCount + 1)
                      : ButtonNavigator::previousIndex(index, settingsCount + 1);
    }
    selectedSettingIndex = index;
    if (selectedSettingIndex == 0) {
      topIndex = 0;
    } else {
      topIndex = followListSelection(selectedSettingIndex - 1, topIndex, visibleRows, settingsCount);
    }
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, &moveSelection] {
    moveSelection(ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1), true);
  });
  buttonNavigator.onPreviousRelease([this, &moveSelection] {
    moveSelection(ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1), false);
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    showSettingSelection = true;
    enterCategory(ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    showSettingSelection = true;
    enterCategory(ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount));
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    setCurrentSettingsForCategory();
    // Advance past any leading section headers
    while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
           (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
      const int nextIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
      if (nextIndex <= selectedSettingIndex) {
        selectedSettingIndex = settingsCount;
        break;
      }
      selectedSettingIndex = nextIndex;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    openLineHeightPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::screenMargin) {
    openScreenMarginPicker(setting);
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::readingIdleTimeThresholdUnits) {
    openIdleTimeThresholdPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ) {
    startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), [this](const ActivityResult&) {
      SETTINGS.saveToFile();
      requestUpdate();
    });
    return;
  }
  if (setting.type == SettingType::STRING) {
    openStringEditor(setting);
    return;
  }
  if (setting.nameId == StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM) {
    startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                           [this](const ActivityResult&) {
                             SETTINGS.saveToFile();
                             rebuildSettingsLists();
                           });
    return;
  }

  if (currentSettingUsesOptionMenu(setting)) {
    openEnumOptionPicker(setting);
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, currentValue);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::RemapFrontButtonsReader:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, true), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::BookOrbitSync:
        startActivityForResult(std::make_unique<BookOrbitSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::BackupStats:
        startActivityForResult(std::make_unique<BackupStatsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ResetGlobalStats:
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_RESET_ALL_TIME_STATS),
                                                   tr(STR_RESET_ALL_TIME_STATS_CONFIRM)),
            [this](const ActivityResult& result) {
              if (!result.isCancelled && !GlobalReadingStats::resetLocal()) {
                LOG_ERR("SET", "Failed to reset all-time reading stats");
              }
              requestUpdate();
            });
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        silentRestartToNetwork(NetworkBootTarget::OTA);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        openLanguagePicker();
        break;
      case SettingAction::ClockSync:
        startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ReaderFontOptions:
      case SettingAction::ReaderPageLayout:
      case SettingAction::ControlsPowerButton:
      case SettingAction::ControlsFrontButtons:
      case SettingAction::ControlsSideButtons:
      case SettingAction::SystemDevice:
      case SettingAction::SystemFilesCache:
      case SettingAction::SystemReadingStats:
      case SettingAction::SystemGlobalStats:
      case SettingAction::DisplaySleepScreen:
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else if (setting.type == SettingType::SUBMENU) {
    openSubmenu(setting.action);
    return;
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  // Apply this while `setting` still refers to the current list; rebuilding
  // below clears its backing vector and invalidates the reference.
  applyUiSettingChange(setting.valuePtr);
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/true,
          /*showPercentValue=*/false, StrId::STR_SLEEP_NEVER, /*overrideDisabledReaderTouchscreen=*/false,
          /*showTouchHeaderBackButton=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openLineHeightPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "LineHeightInterval", StrId::STR_LINE_SPACING, SETTINGS.lineHeightPercent,
          CrossPointSettings::MIN_LINE_HEIGHT_PERCENT, CrossPointSettings::MAX_LINE_HEIGHT_PERCENT, 1, 10,
          StrId::STR_NONE_OPT, /*readerActivity=*/false,
          /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/true,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(
              static_cast<uint8_t>(std::get<IntervalResult>(result.data).value));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openIdleTimeThresholdPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "IdleTimeThresholdInterval", StrId::STR_IDLE_TIME_THRESHOLD,
          SETTINGS.getReadingIdleTimeThresholdSeconds(), CrossPointSettings::MIN_READING_IDLE_TIME_THRESHOLD_SECONDS,
          CrossPointSettings::MAX_READING_IDLE_TIME_THRESHOLD_SECONDS,
          CrossPointSettings::READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS, 60, StrId::STR_SECONDS_VALUE_FORMAT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false,
          /*showPercentValue=*/false, StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false,
          /*showTouchHeaderBackButton=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.readingIdleTimeThresholdUnits = CrossPointSettings::readingIdleTimeThresholdUnitsForSeconds(
              static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (settingShowsNavigationCaret(setting)) return ">";
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    return SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t displayIndex = enumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
    return settingEnumOptionLabel(setting, displayIndex);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    return settingEnumOptionLabel(setting, setting.valueGetter());
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    return formatSettingValue(setting);
  }
  if (setting.type == SettingType::ACTION && setting.action == SettingAction::Language) {
    return I18N.getLanguageName(I18N.getLanguage());
  }
  if (setting.type == SettingType::STRING) {
    if (setting.nameId == StrId::STR_DEVICE_NAME) return SETTINGS.getEffectiveDeviceName();
    if (setting.stringGetter) return setting.stringGetter();
    if (setting.stringMaxLen > 0) return reinterpret_cast<const char*>(&SETTINGS) + setting.stringOffset;
  }
  return "";
}

void SettingsActivity::settingsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<SettingsActivity*>(user)->buildSettingsScreen(screen);
}

void SettingsActivity::buildSettingsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content starts directly below the compact header divider.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(settingsTabBarTop(metrics)), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  // Category tabs. The selected pill dims to a dither when the selection is
  // down in the list (the legacy focused/unfocused tab distinction).
  fui::TabItem tabs[categoryCount];
  for (int i = 0; i < categoryCount; i++) {
    tabs[i].label = I18N.get(categoryNames[i]);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = selectedCategoryIndex == i;
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = categoryCount;
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  // Small text: four category labels share the band, the body font truncates.
  tabProps.text = screen.theme().smallText;
  // Pill wraps the label with real padding; the band grows with the scaled
  // font when the theme's tabBarHeight is too short for it. Keep the horizontal
  // padding tight: four equal slots share the band, so wide labels (e.g.
  // "Controls") truncate to an ellipsis at large UI scales when the pill eats
  // too much width. Vertical padding stays for the pill height.
  tabProps.tabInset = fui::Insets{2, 2, 4, 2};
  tabProps.contentInset = fui::Insets{2, 4, 2, 4};
  const int16_t tabLineHeight = screen.target().lineHeight(screen.theme().smallText.font);
  const int16_t tabBand =
      static_cast<int16_t>(metrics.tabBarHeight > tabLineHeight + 10 ? metrics.tabBarHeight : tabLineHeight + 10);
  // Legacy Lyra two-state treatment: with the selection on the tab band, the
  // band fills gray and the active tab is a solid pill; with the selection
  // down in the list, the band is plain and the active tab keeps a gray box
  // with an underline. The 1px rule under the band is always there.
  const bool tabsFocused = selectedSettingIndex == 0;
  const bool borderedTabs = metrics.tabBarAppearance == ThemeTabBarAppearance::BorderedText;
  const bool roundedRaffTabs = SETTINGS.uiTheme == CrossPointSettings::UI_THEME::ROUNDEDRAFF;
  tabProps.divider = true;
  fui::StyleSet tabStyles;
  if (roundedRaffTabs) {
    // RoundedRaff's tabs have always sat on white, with the selected pill
    // turning dark gray after focus moves into the settings list.
    tabStyles.explicitlySet = true;
    tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.background = fui::Paint::solid(tabsFocused ? fui::Color::Black : fui::Color::DarkGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = 18;
    tabStyles.focused = tabStyles.selected;
    tabStyles.active = tabStyles.selected;
    tabProps.tabStyles = tabStyles;
  } else if (!borderedTabs) {
    tabStyles.explicitlySet = true;
    tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    if (tabsFocused) {
      tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
      tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
      tabStyles.selected.radius = screen.theme().listRowRadius;
    } else {
      tabStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
      tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
      tabProps.selectedUnderline = 2;
    }
    // Focus/flash states keep the pill instead of falling back to an unset
    // (blank) style.
    tabStyles.focused = tabStyles.selected;
    tabStyles.active = tabStyles.selected;
    tabProps.tabStyles = tabStyles;
  }
  const fui::Rect tabRect = screen.takeTop(tabBand);
  if (!roundedRaffTabs && !borderedTabs && tabsFocused) {
    screen.target().fill(tabRect, fui::Paint::dither(fui::Color::LightGray));
  }
  drawUiTabBar(screen, tabProps, tabRect, metrics.tabBarAppearance);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const StrId submenuTitle = activeSubmenuTitleId();
  if (submenuTitle != StrId::STR_NONE_OPT) {
    fui::TextStyle titleStyle = screen.theme().smallText;
    titleStyle.bold = true;
    titleStyle.maxLines = 1;
    const int16_t titleHeight = screen.target().lineHeight(titleStyle.font);
    fui::Rect titleRect = screen.takeTop(titleHeight, static_cast<int16_t>(metrics.verticalSpacing));
    const int16_t sidePadding = static_cast<int16_t>(metrics.contentSidePadding);
    titleRect.x = static_cast<int16_t>(titleRect.x + sidePadding);
    titleRect.width = static_cast<int16_t>(titleRect.width > sidePadding * 2 ? titleRect.width - sidePadding * 2 : 0);
    screen.target().text(titleRect, I18N.get(submenuTitle), titleStyle);
  }

  // Settings rows. Values are built per render and owned for the draw only.
  const auto& settings = *currentSettings;
  std::vector<std::string> values(settings.size());
  std::vector<fui::ListItem> items;
  items.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    values[i] = settingValueText(settings[i]);
    const bool isSectionHeader = settings[i].type == SettingType::SECTION_HEADER;
    fui::ListItem item;
    item.label = isSectionHeader ? uiListSectionHeaderLabel(values[i], I18N.get(settings[i].nameId))
                                 : I18N.get(settings[i].nameId);
    if (!isSectionHeader && !values[i].empty()) item.value = values[i].c_str();
    item.isHeader = isSectionHeader;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedSettingIndex - 1);  // -1 = tab band focused
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Use FreeInkUI's bodyText default so Settings scales consistently with
  // File Browser, reader menus, and the other list-style screens.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  configureUiListSectionHeaders(props, screen.theme());
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, settingsCount);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::drawCompact(renderer, tr(STR_SETTINGS_TITLE), false, true);
  } else {
    CompactHeader::drawTitle(renderer, tr(STR_SETTINGS_TITLE), true);
  }

  uiReady = false;
  app.render();
  uiReady = true;

  // Keep build information discoverable without crowding the common header.
  if (selectedCategoryIndex == 3) {
    drawSystemVersionFooter(renderer, pageWidth, renderer.getScreenHeight(), metrics);
  }

  const auto confirmLabel =
      (selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingIndex > 0 &&
                     (currentSettingUsesOptionMenu((*currentSettings)[selectedSettingIndex - 1]) ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SUBMENU ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::ACTION ||
                      (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_FONT_FAMILY ||
                      (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::STRING ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr == &CrossPointSettings::lineHeightPercent ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr ==
                          &CrossPointSettings::readingIdleTimeThresholdUnits ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr == &CrossPointSettings::screenMargin)
                 ? tr(STR_SELECT)
                 : tr(STR_TOGGLE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
