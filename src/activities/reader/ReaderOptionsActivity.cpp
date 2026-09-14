#include "ReaderOptionsActivity.h"

#include <Epub/EpubRenderMode.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "util/DictionaryRegistry.h"

namespace fui = freeink::ui;

namespace {
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

std::string formatSettingValue(const SettingInfo& setting) {
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    return std::to_string(SETTINGS.*(setting.valuePtr)) + "%";
  }
  return std::to_string(SETTINGS.*(setting.valuePtr));
}

SettingInfo buildReaderRenderModeSetting() {
  return SettingInfo::Enum(
             StrId::STR_EPUB_RENDER_MODE, &CrossPointSettings::epubRenderMode,
             {StrId::STR_RENDER_MODE_CROSSINK_DEFAULT, StrId::STR_RENDER_MODE_BALANCED, StrId::STR_RENDER_MODE_LIGHT})
      .withEnumRawValues({static_cast<uint8_t>(EpubRenderMode::CrossInkDefault),
                          static_cast<uint8_t>(EpubRenderMode::Balanced), static_cast<uint8_t>(EpubRenderMode::Light)});
}
}  // namespace

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();

  activeSubmenu = SettingAction::None;
  settingsDirty = false;
  rebuildSettingsList();
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &ReaderOptionsActivity::onRowEvent, this);
  app.setScreen(&ReaderOptionsActivity::optionsScreen, this);
  requestUpdate();
}

void ReaderOptionsActivity::rebuildSettingsList() {
  settings.clear();
  fontSettings.clear();
  pageLayoutSettings.clear();
  screenMarginSettings.clear();
  sdFontSystem.refreshIfDirty();
  const auto allSettings = getSettingsList(&sdFontSystem.registry());
  settings = buildBookReaderSettingsParentList(allSettings);
  const auto indexingMethod = std::find_if(settings.begin(), settings.end(), [](const SettingInfo& setting) {
    return setting.nameId == StrId::STR_INDEXING_METHOD;
  });
  if (indexingMethod == settings.end()) {
    settings.push_back(buildReaderRenderModeSetting());
  } else {
    settings.insert(indexingMethod, buildReaderRenderModeSetting());
  }
  fontSettings = buildReaderFontSettingsList(allSettings);
  pageLayoutSettings = buildReaderPageLayoutSettingsList(allSettings);
  screenMarginSettings = buildReaderScreenMarginSettingsList(allSettings);
  fontSettings.erase(std::remove_if(fontSettings.begin(), fontSettings.end(),
                                    [](const SettingInfo& setting) {
                                      return setting.nameId == StrId::STR_SD_FONT_SIZE_RANGE ||
                                             setting.nameId == StrId::STR_DOWNLOAD_FONTS;
                                    }),
                     fontSettings.end());

  // Dictionary-specific font controls are useful only when an installed dictionary can use them.
  DictionaryRegistry installedDictionaryRegistry;
  const bool hasInstalledDictionaries = installedDictionaryRegistry.discover();
  installedDictionaryRegistry.clear();
  if (!hasInstalledDictionaries) {
    setCurrentSettings();
    selectedIndex = 0;
    return;
  }

  SettingInfo dictionaryFont;
  dictionaryFont.nameId = StrId::STR_DICTIONARY_FONT;
  dictionaryFont.type = SettingType::ENUM;
  dictionaryFont.key = "dictionaryFont";
  dictionaryFont.category = StrId::STR_CAT_READER;
  const auto& families = sdFontSystem.registry().getFamilies();
  dictionaryFont.enumStringValues.reserve(families.size() + 2);
  dictionaryFont.enumStringValues.push_back(tr(STR_DICT_USE_GLOBAL));
  bool selectedFamilyIsInstalled = false;
  for (const auto& family : families) {
    dictionaryFont.enumStringValues.push_back(family.name);
    if (family.name == dictionaryFontFamilyName) {
      selectedFamilyIsInstalled = true;
    }
  }
  if (dictionaryFontFamilyName[0] != '\0' && !selectedFamilyIsInstalled) {
    dictionaryFont.enumStringValues.push_back(std::string(dictionaryFontFamilyName) + " (" + tr(STR_UNAVAILABLE) + ")");
  }
  const auto fontSize = std::find_if(fontSettings.begin(), fontSettings.end(),
                                     [](const SettingInfo& setting) { return setting.nameId == StrId::STR_FONT_SIZE; });
  const size_t dictionaryFontIndex =
      fontSize == fontSettings.end() ? 0 : static_cast<size_t>(std::distance(fontSettings.begin(), fontSize) + 1);
  fontSettings.insert(fontSettings.begin() + dictionaryFontIndex, std::move(dictionaryFont));

  SettingInfo dictionaryFontSize;
  dictionaryFontSize.nameId = StrId::STR_DICTIONARY_FONT_SIZE;
  dictionaryFontSize.type = SettingType::ENUM;
  dictionaryFontSize.key = "dictionaryFontSize";
  dictionaryFontSize.category = StrId::STR_CAT_READER;
  dictionaryFontSize.enumStringValues.push_back(tr(STR_DICT_USE_GLOBAL));
  dictionaryFontSize.enumRawValues.push_back(0);
  const char* dictionarySizeFamily =
      dictionaryFontFamilyName[0] != '\0' ? dictionaryFontFamilyName : SETTINGS.sdFontFamilyName;
  if (dictionarySizeFamily[0] != '\0') {
    if (const auto* family = sdFontSystem.registry().findFamily(dictionarySizeFamily)) {
      dictionaryFontSize.enumStringValues.reserve(family->files.size() + 1);
      dictionaryFontSize.enumRawValues.reserve(family->files.size() + 1);
      // Build a sorted, unique list directly from the catalog already resident
      // for this screen. Dictionary lookup itself never retains a size list.
      for (const auto& file : family->files) {
        if (file.style != 0 ||
            std::find(dictionaryFontSize.enumRawValues.begin(), dictionaryFontSize.enumRawValues.end(),
                      file.pointSize) != dictionaryFontSize.enumRawValues.end()) {
          continue;
        }
        const auto insertAt = std::lower_bound(dictionaryFontSize.enumRawValues.begin() + 1,
                                               dictionaryFontSize.enumRawValues.end(), file.pointSize);
        const size_t index = static_cast<size_t>(std::distance(dictionaryFontSize.enumRawValues.begin(), insertAt));
        dictionaryFontSize.enumRawValues.insert(insertAt, file.pointSize);
        dictionaryFontSize.enumStringValues.insert(dictionaryFontSize.enumStringValues.begin() + index,
                                                   fontSizePointLabel(file.pointSize));
      }
    }
  }
  fontSettings.insert(fontSettings.begin() + dictionaryFontIndex + 1, std::move(dictionaryFontSize));

  setCurrentSettings();
  selectedIndex = 0;
}

void ReaderOptionsActivity::persistReaderSettings() {
  if (saveSettingsCallback) {
    saveSettingsCallback(saveSettingsContext);
  } else {
    SETTINGS.saveToFile();
  }
}

void ReaderOptionsActivity::persistGlobalSettings() {
  if (saveGlobalSettingsCallback) {
    saveGlobalSettingsCallback(saveGlobalSettingsContext);
  } else {
    SETTINGS.saveToFile();
  }
}

void ReaderOptionsActivity::beginGlobalSettingsEdit() {
  if (beginGlobalSettingsEditCallback) {
    beginGlobalSettingsEditCallback(beginGlobalSettingsEditContext);
  }
}

void ReaderOptionsActivity::endGlobalSettingsEdit() {
  if (endGlobalSettingsEditCallback) {
    endGlobalSettingsEditCallback(endGlobalSettingsEditContext);
  }
}

void ReaderOptionsActivity::setCurrentSettings() {
  switch (activeSubmenu) {
    case SettingAction::ReaderFontOptions:
      currentSettings = &fontSettings;
      break;
    case SettingAction::ReaderPageLayout:
      currentSettings = &pageLayoutSettings;
      break;
    case SettingAction::ScreenMargin:
      currentSettings = &screenMarginSettings;
      break;
    default:
      currentSettings = &settings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

StrId ReaderOptionsActivity::activeSubmenuTitleId() const {
  switch (activeSubmenu) {
    case SettingAction::ReaderFontOptions:
      return StrId::STR_READER_FONT_OPTIONS;
    case SettingAction::ReaderPageLayout:
      return StrId::STR_READER_PAGE_LAYOUT;
    case SettingAction::ScreenMargin:
      return StrId::STR_SCREEN_MARGIN;
    default:
      return StrId::STR_NONE_OPT;
  }
}

void ReaderOptionsActivity::openSubmenu(SettingAction action) {
  parentSubmenu = activeSubmenu;
  activeSubmenu = action;
  setCurrentSettings();
  selectedIndex = 0;
  topIndex = 0;
}

void ReaderOptionsActivity::closeSubmenu() {
  activeSubmenu = parentSubmenu;
  parentSubmenu = SettingAction::None;
  setCurrentSettings();
  selectedIndex = 0;
  topIndex = 0;
}

void ReaderOptionsActivity::onExit() {
  sdFontSystem.releaseRegistry();
  Activity::onExit();
}

void ReaderOptionsActivity::moveSelection(bool forward) {
  if (settingsCount <= 0) return;

  for (int i = 0; i < settingsCount; i++) {
    selectedIndex = forward ? ButtonNavigator::nextIndex(selectedIndex, settingsCount)
                            : ButtonNavigator::previousIndex(selectedIndex, settingsCount);
    if ((*currentSettings)[selectedIndex].type != SettingType::SECTION_HEADER) {
      topIndex = followListSelection(selectedIndex, topIndex, visibleRows, settingsCount);
      break;
    }
  }
}

bool ReaderOptionsActivity::currentSettingUsesOptionMenu(const SettingInfo& setting) const {
  return setting.nameId != StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM &&
         settingEnumOptionCount(setting) > 2 &&
         (setting.valuePtr != nullptr || (setting.valueGetter && setting.valueSetter));
}

void ReaderOptionsActivity::openEnumOptionPicker(const SettingInfo& setting) {
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
  const auto note = setting.valuePtr == &CrossPointSettings::sideButtonChordAction && mappedInput.hasTouchHardware()
                        ? OptionPopup::Note{tr(STR_NOTE), tr(STR_TOUCHSCREEN_ESCAPE_HATCH_NOTE)}
                        : OptionPopup::Note{};
  optionPopup.show(
      setting.nameId, options, currentIndex,
      [this, selectedSetting](int selectedIndex) {
        if (selectedSetting.valuePtr != nullptr) {
          SETTINGS.*(selectedSetting.valuePtr) =
              enumRawValueForDisplayIndex(selectedSetting, static_cast<uint8_t>(selectedIndex));
        } else if (selectedSetting.valueSetter) {
          selectedSetting.valueSetter(static_cast<uint8_t>(selectedIndex));
        }

        persistReaderSettings();
        requestUpdate();
      },
      note);
  requestUpdate();
}

void ReaderOptionsActivity::openDictionaryFontPicker(const SettingInfo& setting) {
  const size_t optionCount = setting.enumStringValues.size();
  if (optionCount == 0) return;

  uint8_t currentIndex = 0;
  if (hasDictionaryFontOverride && dictionaryFontFamilyName[0] != '\0') {
    for (size_t i = 1; i < optionCount; ++i) {
      if (setting.enumStringValues[i] == dictionaryFontFamilyName) {
        currentIndex = static_cast<uint8_t>(i);
        break;
      }
    }
    if (currentIndex == 0) currentIndex = static_cast<uint8_t>(optionCount - 1);
  }

  const SettingInfo selectedSetting = setting;
  optionPopup.show(setting.nameId, selectedSetting.enumStringValues, currentIndex, [this, selectedSetting](int index) {
    const char* familyName = nullptr;
    if (index > 0 && index < static_cast<int>(selectedSetting.enumStringValues.size())) {
      const std::string& candidate = selectedSetting.enumStringValues[static_cast<size_t>(index)];
      // An unavailable entry is only the retained current value. Keep it rather
      // than saving its decorated display label.
      familyName = sdFontSystem.registry().findFamily(candidate.c_str()) ? candidate.c_str() : dictionaryFontFamilyName;
    }

    // ReaderOptions owns this name so the EPUB reader does not retain the
    // extra 64 bytes during section layout. Update that owned copy before
    // rebuilding the screen; otherwise it redraws the previously selected
    // family even though the new value was persisted successfully.
    if (familyName && familyName[0] != '\0') {
      hasDictionaryFontOverride = true;
      if (familyName != dictionaryFontFamilyName) {
        std::strncpy(dictionaryFontFamilyName, familyName, sizeof(dictionaryFontFamilyName) - 1);
        dictionaryFontFamilyName[sizeof(dictionaryFontFamilyName) - 1] = '\0';
      }
    } else {
      hasDictionaryFontOverride = false;
      std::strncpy(dictionaryFontFamilyName, SETTINGS.dictionarySdFontFamilyName, sizeof(dictionaryFontFamilyName) - 1);
      dictionaryFontFamilyName[sizeof(dictionaryFontFamilyName) - 1] = '\0';
      dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
    }
    if (dictionaryFontChangedCallback) {
      dictionaryFontChangedCallback(dictionaryFontChangedContext,
                                    hasDictionaryFontOverride ? dictionaryFontFamilyName : nullptr,
                                    dictionaryFontPointSize);
    }
    rebuildSettingsList();
    requestUpdate();
  });
  requestUpdate();
}

void ReaderOptionsActivity::openDictionaryFontSizePicker(const SettingInfo& setting) {
  const size_t optionCount = setting.enumRawValues.size();
  if (optionCount == 0) return;

  uint8_t currentIndex = hasDictionaryFontOverride ? enumDisplayIndexForRawValue(setting, dictionaryFontPointSize) : 0;
  if (currentIndex >= optionCount) currentIndex = 0;
  const SettingInfo selectedSetting = setting;
  optionPopup.show(setting.nameId, selectedSetting.enumStringValues, currentIndex, [this, selectedSetting](int index) {
    const uint8_t pointSize = enumRawValueForDisplayIndex(selectedSetting, static_cast<uint8_t>(index));
    if (!hasDictionaryFontOverride) {
      if (index == 0) {
        dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
      } else if (dictionaryFontFamilyName[0] != '\0') {
        hasDictionaryFontOverride = true;
        dictionaryFontPointSize = pointSize;
      } else if (SETTINGS.sdFontFamilyName[0] != '\0') {
        // Persist the reader family explicitly for this book so its size can
        // differ from the global dictionary default after the menu closes.
        std::strncpy(dictionaryFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(dictionaryFontFamilyName) - 1);
        dictionaryFontFamilyName[sizeof(dictionaryFontFamilyName) - 1] = '\0';
        hasDictionaryFontOverride = true;
        dictionaryFontPointSize = pointSize;
      }
    } else {
      dictionaryFontPointSize = pointSize;
    }
    if (dictionaryFontChangedCallback) {
      dictionaryFontChangedCallback(dictionaryFontChangedContext,
                                    hasDictionaryFontOverride ? dictionaryFontFamilyName : nullptr,
                                    dictionaryFontPointSize);
    }
    requestUpdate();
  });
  requestUpdate();
}

void ReaderOptionsActivity::openScreenMarginPicker(const SettingInfo& setting) {
  if (setting.valuePtr == nullptr) return;

  const SettingInfo selectedSetting = setting;
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "ReaderOptionsScreenMarginInterval", selectedSetting.nameId,
          SETTINGS.*(selectedSetting.valuePtr), CrossPointSettings::MIN_SCREEN_MARGIN,
          CrossPointSettings::MAX_SCREEN_MARGIN, CrossPointSettings::SCREEN_MARGIN_SMALL_STEP,
          CrossPointSettings::SCREEN_MARGIN_LARGE_STEP, StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/false,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true,
          /*valueFormatter=*/nullptr, /*tapStep=*/5, /*useReaderSlider=*/true),
      [this, selectedSetting](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.*(selectedSetting.valuePtr) = static_cast<uint8_t>(std::clamp(
              std::get<IntervalResult>(result.data).value, static_cast<uint32_t>(CrossPointSettings::MIN_SCREEN_MARGIN),
              static_cast<uint32_t>(CrossPointSettings::MAX_SCREEN_MARGIN)));
          persistReaderSettings();
        }
        requestUpdate();
      });
}

void ReaderOptionsActivity::openWordSpacingPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "ReaderOptionsWordSpacingInterval", StrId::STR_WORD_SPACING, SETTINGS.wordSpacing, 0,
          CrossPointSettings::MAX_WORD_SPACING, 1, 1, StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/false,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true,
          /*valueFormatter=*/nullptr, /*tapStep=*/1, /*useReaderSlider=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.wordSpacing =
              static_cast<uint8_t>(std::clamp(std::get<IntervalResult>(result.data).value, static_cast<uint32_t>(0),
                                              static_cast<uint32_t>(CrossPointSettings::MAX_WORD_SPACING)));
          persistReaderSettings();
        }
        requestUpdate();
      });
}

void ReaderOptionsActivity::toggleCurrentSetting() {
  if (selectedIndex < 0 || selectedIndex >= settingsCount) return;
  const auto& setting = (*currentSettings)[selectedIndex];

  if (setting.nameId == StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM) {
    startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               persistReaderSettings();
                             }
                             sdFontSystem.refreshIfDirty();
                             rebuildSettingsList();
                             requestUpdate();
                           });
    return;
  }

  if (setting.nameId == StrId::STR_DICTIONARY_FONT && setting.type == SettingType::ENUM) {
    openDictionaryFontPicker(setting);
    return;
  }

  if (setting.nameId == StrId::STR_DICTIONARY_FONT_SIZE && setting.type == SettingType::ENUM) {
    openDictionaryFontSizePicker(setting);
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !cur;
    settingsDirty = true;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    if (currentSettingUsesOptionMenu(setting)) {
      openEnumOptionPicker(setting);
      return;
    }
    const uint8_t cur = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, cur);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
    settingsDirty = true;
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (currentSettingUsesOptionMenu(setting)) {
      openEnumOptionPicker(setting);
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
    settingsDirty = true;
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
      openLineHeightPicker();
      return;
    }
    if (setting.valuePtr == &CrossPointSettings::wordSpacing) {
      openWordSpacingPicker();
      return;
    }
    if (setting.valuePtr == &CrossPointSettings::screenMarginVertical ||
        setting.valuePtr == &CrossPointSettings::screenMarginHorizontal) {
      openScreenMarginPicker(setting);
      return;
    }
    const int8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = cur + setting.valueRange.step;
    }
    settingsDirty = true;
  } else if (setting.type == SettingType::ACTION) {
    if (setting.action == SettingAction::DownloadFonts) {
      if (settingsDirty) {
        persistReaderSettings();
        settingsDirty = false;
      }
      silentRestartToManageFonts();
      return;
    }
    if (setting.action == SettingAction::CustomiseStatusBar) {
      if (settingsDirty) {
        persistReaderSettings();
        settingsDirty = false;
      }
      beginGlobalSettingsEdit();
      startActivityForResult(
          std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput, true, stablePageNumbersAvailable),
          [this](const ActivityResult&) {
            persistGlobalSettings();
            endGlobalSettingsEdit();
          });
      return;
    }
  } else if (setting.type == SettingType::SUBMENU) {
    openSubmenu(setting.action);
    return;
  }
}

void ReaderOptionsActivity::openLineHeightPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "ReaderOptionsLineHeightInterval", StrId::STR_LINE_SPACING, SETTINGS.lineHeightPercent,
          CrossPointSettings::MIN_LINE_HEIGHT_PERCENT, CrossPointSettings::MAX_LINE_HEIGHT_PERCENT, 1, 5,
          StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/true,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true,
          /*valueFormatter=*/nullptr, /*tapStep=*/5, /*useReaderSlider=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(
              static_cast<uint8_t>(std::get<IntervalResult>(result.data).value));
          persistReaderSettings();
        }
        requestUpdate();
      });
}

void ReaderOptionsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
      return;
    }
    if (settingsDirty) {
      persistReaderSettings();
      settingsDirty = false;
    }
    finish();
    return;
  }
  if (mappedInput.wasHomeGesture()) {
    if (settingsDirty) {
      persistReaderSettings();
      settingsDirty = false;
    }
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  if (mappedInput.hasTouch()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      const int next = scrollListBy(topIndex, visibleRows, visibleRows, settingsCount);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      const int next = scrollListBy(topIndex, -visibleRows, visibleRows, settingsCount);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
  }

  buttonNavigator.onNextRelease([this] {
    moveSelection(true);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    moveSelection(false);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
      return;
    }
    if (settingsDirty) {
      persistReaderSettings();
      settingsDirty = false;
    }
    finish();
    return;
  }
}

void ReaderOptionsActivity::optionsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<ReaderOptionsActivity*>(user)->buildOptionsScreen(screen);
}

void ReaderOptionsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderOptionsActivity*>(user);
  if (self->optionPopup.isActive() || event.value < 0 || event.value >= self->settingsCount) return;
  if ((*self->currentSettings)[event.value].type == SettingType::SECTION_HEADER) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->toggleCurrentSetting();
}

void ReaderOptionsActivity::buildOptionsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const StrId submenuTitleId = activeSubmenuTitleId();
  if (submenuTitleId != StrId::STR_NONE_OPT) {
    fui::TextStyle titleStyle = screen.theme().smallText;
    titleStyle.bold = true;
    titleStyle.maxLines = 1;
    const int16_t titleHeight = screen.target().lineHeight(titleStyle.font);
    fui::Rect titleRect = screen.takeTop(titleHeight, static_cast<int16_t>(metrics.verticalSpacing));
    const int16_t sidePadding = static_cast<int16_t>(metrics.contentSidePadding);
    titleRect.x = static_cast<int16_t>(titleRect.x + sidePadding);
    titleRect.width = static_cast<int16_t>(titleRect.width > sidePadding * 2 ? titleRect.width - sidePadding * 2 : 0);
    screen.target().text(titleRect, I18N.get(submenuTitleId), titleStyle);
  }

  const auto& currentSettingsList = *currentSettings;
  std::vector<std::string> values(currentSettingsList.size());
  std::vector<fui::ListItem> items;
  items.reserve(currentSettingsList.size());
  for (size_t i = 0; i < currentSettingsList.size(); ++i) {
    const auto& setting = currentSettingsList[i];
    if (settingShowsNavigationCaret(setting)) {
      values[i] = ">";
    } else if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
      values[i] = SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
      const uint8_t displayValue = enumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
      values[i] = settingEnumOptionLabel(setting, displayValue < settingEnumOptionCount(setting) ? displayValue : 0);
    } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
      values[i] = settingEnumOptionLabel(setting, setting.valueGetter());
    } else if (setting.nameId == StrId::STR_DICTIONARY_FONT) {
      if (!hasDictionaryFontOverride) {
        values[i] = tr(STR_DICT_USE_GLOBAL);
        if (dictionaryFontFamilyName[0] != '\0') {
          values[i] += " (";
          values[i] += dictionaryFontFamilyName;
          values[i] += ')';
        }
      } else if (dictionaryFontFamilyName[0] != '\0') {
        values[i] = dictionaryFontFamilyName;
        if (!sdFontSystem.registry().findFamily(dictionaryFontFamilyName)) {
          values[i] += " (";
          values[i] += tr(STR_UNAVAILABLE);
          values[i] += ')';
        }
      } else {
        values[i] = tr(STR_USE_READER_FONT);
      }
    } else if (setting.nameId == StrId::STR_DICTIONARY_FONT_SIZE) {
      if (!hasDictionaryFontOverride) {
        values[i] = tr(STR_DICT_USE_GLOBAL);
        if (dictionaryFontPointSize != 0) {
          values[i] += " (";
          values[i] += fontSizePointLabel(dictionaryFontPointSize);
          values[i] += ')';
        }
      } else {
        const uint8_t displayValue = enumDisplayIndexForRawValue(setting, dictionaryFontPointSize);
        values[i] = settingEnumOptionLabel(setting, displayValue < settingEnumOptionCount(setting) ? displayValue : 0);
      }
    } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
      values[i] = formatSettingValue(setting);
    }

    const bool isSectionHeader = setting.type == SettingType::SECTION_HEADER;
    fui::ListItem item;
    item.label =
        isSectionHeader ? uiListSectionHeaderLabel(values[i], I18N.get(setting.nameId)) : I18N.get(setting.nameId);
    if (!isSectionHeader && !values[i].empty()) item.value = values[i].c_str();
    item.isHeader = isSectionHeader;
    item.toggle = !isSectionHeader && setting.type == SettingType::TOGGLE;
    if (item.toggle) {
      item.toggleChecked = setting.valuePtr != nullptr && SETTINGS.*(setting.valuePtr) != 0;
      item.value = nullptr;
    }
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  configureUiListSectionHeaders(props, screen.theme());
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, settingsCount);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void ReaderOptionsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  header.x = safe.x;
  header.width = safe.width;
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_READER_OPTIONS), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_READER_OPTIONS), nullptr, true);
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const bool currentIsAction = selectedIndex >= 0 && selectedIndex < settingsCount &&
                               ((*currentSettings)[selectedIndex].type == SettingType::ACTION ||
                                (*currentSettings)[selectedIndex].type == SettingType::SUBMENU ||
                                (*currentSettings)[selectedIndex].nameId == StrId::STR_FONT_FAMILY ||
                                (*currentSettings)[selectedIndex].nameId == StrId::STR_DICTIONARY_FONT ||
                                (*currentSettings)[selectedIndex].nameId == StrId::STR_DICTIONARY_FONT_SIZE ||
                                currentSettingUsesOptionMenu((*currentSettings)[selectedIndex]));
  const bool selectedLineHeight = selectedIndex >= 0 && selectedIndex < settingsCount &&
                                  (*currentSettings)[selectedIndex].valuePtr == &CrossPointSettings::lineHeightPercent;
  const bool selectedScreenMargin =
      selectedIndex >= 0 && selectedIndex < settingsCount &&
      ((*currentSettings)[selectedIndex].valuePtr == &CrossPointSettings::screenMarginVertical ||
       (*currentSettings)[selectedIndex].valuePtr == &CrossPointSettings::screenMarginHorizontal);
  const auto labels = mappedInput.mapLabels(
      mappedInput.withBackArrow(tr(STR_BACK)),
      (currentIsAction || selectedLineHeight || selectedScreenMargin) ? tr(STR_SELECT) : tr(STR_TOGGLE), tr(STR_DIR_UP),
      tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
