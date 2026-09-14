#pragma once
#include <Epub.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "ControlsOptionsActivity.h"
#include "EpubReaderMenuModel.h"
#include "ReaderOptionsActivity.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

struct Rect;

class EpubReaderMenuActivity final : public Activity {
 public:
  using MenuAction = EpubReaderMenuAction;

  explicit EpubReaderMenuActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
      const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
      const bool hasDictionary, const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked,
      const bool isBookCompleted, const bool autoPageTurnActive = false, const uint16_t autoPageTurnIntervalSeconds = 0,
      const bool showReadingPaceReset = false,
      ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback = nullptr,
      void* saveReaderSettingsContext = nullptr,
      ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr,
      void* saveGlobalSettingsContext = nullptr,
      ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr,
      void* beginGlobalSettingsEditContext = nullptr, bool stablePageNumbersAvailable = false,
      ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr,
      void* endGlobalSettingsEditContext = nullptr, const char* dictionaryFontFamilyName = nullptr,
      uint8_t dictionaryFontPointSize = 0, bool hasDictionaryFontOverride = false,
      ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr,
      void* dictionaryFontChangedContext = nullptr);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowFrontlightPanelGesture() const override { return false; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool allowGlobalHomeGesture() const override { return false; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  enum class MenuTab : uint8_t { Main = 0, Bookmarks = 1, Settings = 2 };
  static constexpr size_t MAIN_TAB_INDEX = 0;
  static constexpr size_t BOOKMARKS_TAB_INDEX = 1;
  static constexpr size_t SETTINGS_TAB_INDEX = 2;
  static constexpr size_t MENU_TAB_COUNT = 3;
  static constexpr size_t TOUCH_LOCK_ICON_INDEX = MENU_TAB_COUNT;
  static constexpr size_t TOUCH_HOME_ICON_INDEX = MENU_TAB_COUNT + 1;
  static constexpr size_t TOUCH_ICON_COUNT = MENU_TAB_COUNT + 1;
  using TabMenuItems = std::array<std::vector<MenuItem>, MENU_TAB_COUNT>;

  static TabMenuItems buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool hasClippings,
                                     bool isCurrentPageBookmarked, bool isBookCompleted, bool showReadingPaceReset,
                                     bool hasDictionary);
  [[nodiscard]] const std::vector<MenuItem>& activeMenuItems() const;
  [[nodiscard]] size_t activeTabIndex() const { return static_cast<size_t>(activeTab); }
  void cycleActiveTab();
  void moveActiveTab(bool forward);
  void focusTabRow();
  void finishCancelled();
  bool activateSelectedItem();
  bool handleTouchInput();
  void drawIconTabBar(Rect rect, bool drawBottomBorder);
  static void dictionaryFontChangedForMenu(void* ctx, const char* familyName, uint8_t pointSize);

  // FreeInkApp hosts the menu list (themed rows, touch routing); the header
  // stays on GUI.drawHeader for the battery indicator, and OptionPopup keeps
  // its legacy overlay rendering.
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  static void menuScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildMenuScreen(UiApp::ScreenType& screen);

  // Fixed menu layout, except for rows tied to toggles inside this menu.
  TabMenuItems menuItems;

  int selectedIndex = -1;
  MenuTab activeTab = MenuTab::Main;

  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  bool autoPageTurnActive = false;
  uint16_t autoPageTurnIntervalSeconds = 0;
  ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback = nullptr;
  void* saveReaderSettingsContext = nullptr;
  ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  bool stablePageNumbersAvailable = false;
  ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  char dictionaryFontFamilyName[64] = "";
  uint8_t dictionaryFontPointSize = 0;
  bool hasDictionaryFontOverride = false;
  ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr;
  void* dictionaryFontChangedContext = nullptr;
  bool settingsChanged = false;
  ReaderSettingsChangeMask changeMask = ReaderSettingsChangeMask::None;

  MenuResult makeMenuResult(int action) const;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
};
