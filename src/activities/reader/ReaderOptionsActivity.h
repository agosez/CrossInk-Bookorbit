#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "components/OptionPopup.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/ButtonNavigator.h"

class ReaderOptionsActivity final : public Activity {
 public:
  using SaveSettingsCallback = void (*)(void* ctx);
  using SaveGlobalSettingsCallback = void (*)(void* ctx);
  using GlobalSettingsEditCallback = void (*)(void* ctx);
  using DictionaryFontChangedCallback = void (*)(void* ctx, const char* familyName, uint8_t pointSize);

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int settingsCount = 0;
  std::vector<SettingInfo> settings;
  std::vector<SettingInfo> fontSettings;
  std::vector<SettingInfo> pageLayoutSettings;
  std::vector<SettingInfo> screenMarginSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;
  SettingAction activeSubmenu = SettingAction::None;
  SettingAction parentSubmenu = SettingAction::None;
  OptionPopup optionPopup;
  SaveSettingsCallback saveSettingsCallback = nullptr;
  void* saveSettingsContext = nullptr;
  SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  char dictionaryFontFamilyName[64] = "";
  uint8_t dictionaryFontPointSize = 0;
  bool hasDictionaryFontOverride = false;
  DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr;
  void* dictionaryFontChangedContext = nullptr;
  bool settingsDirty = false;
  bool stablePageNumbersAvailable = false;

  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  freeink::ui::GfxRendererTarget uiTarget;  // Must precede app: the app holds a reference to it.
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  void rebuildSettingsList();
  void setCurrentSettings();
  StrId activeSubmenuTitleId() const;
  void openSubmenu(SettingAction action);
  void closeSubmenu();
  void moveSelection(bool forward);
  bool currentSettingUsesOptionMenu(const SettingInfo& setting) const;
  void openEnumOptionPicker(const SettingInfo& setting);
  void openDictionaryFontPicker(const SettingInfo& setting);
  void openDictionaryFontSizePicker(const SettingInfo& setting);
  void openScreenMarginPicker(const SettingInfo& setting);
  void openWordSpacingPicker();
  void toggleCurrentSetting();
  void openLineHeightPicker();
  void persistReaderSettings();
  void persistGlobalSettings();
  void beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();
  static void optionsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildOptionsScreen(UiApp::ScreenType& screen);

 public:
  explicit ReaderOptionsActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, SaveSettingsCallback saveSettingsCallback = nullptr,
      void* saveSettingsContext = nullptr, SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr,
      void* saveGlobalSettingsContext = nullptr, GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr,
      void* beginGlobalSettingsEditContext = nullptr,
      GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr, void* endGlobalSettingsEditContext = nullptr,
      bool stablePageNumbersAvailable = false, const char* dictionaryFontFamilyName = nullptr,
      uint8_t dictionaryFontPointSize = 0, bool hasDictionaryFontOverride = false,
      DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr,
      void* dictionaryFontChangedContext = nullptr)
      : Activity("ReaderOptions", renderer, mappedInput),
        saveSettingsCallback(saveSettingsCallback),
        saveSettingsContext(saveSettingsContext),
        saveGlobalSettingsCallback(saveGlobalSettingsCallback),
        saveGlobalSettingsContext(saveGlobalSettingsContext),
        beginGlobalSettingsEditCallback(beginGlobalSettingsEditCallback),
        beginGlobalSettingsEditContext(beginGlobalSettingsEditContext),
        endGlobalSettingsEditCallback(endGlobalSettingsEditCallback),
        endGlobalSettingsEditContext(endGlobalSettingsEditContext),
        dictionaryFontChangedCallback(dictionaryFontChangedCallback),
        dictionaryFontChangedContext(dictionaryFontChangedContext),
        dictionaryFontPointSize(dictionaryFontPointSize),
        hasDictionaryFontOverride(hasDictionaryFontOverride),
        stablePageNumbersAvailable(stablePageNumbersAvailable),
        uiTarget(makeUiTarget(renderer)),
        app(uiTarget, uiTarget.deviceContext()) {
    if (dictionaryFontFamilyName) {
      std::strncpy(this->dictionaryFontFamilyName, dictionaryFontFamilyName,
                   sizeof(this->dictionaryFontFamilyName) - 1);
    }
  }
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool allowGlobalHomeGesture() const override { return false; }
};
