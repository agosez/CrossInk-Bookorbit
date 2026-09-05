#ifdef SIMULATOR

#include "SimulatorSmokeTest.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingsList.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderMenuActivity.h"
#include "activities/reader/ReaderOptionsActivity.h"
#include "activities/settings/QuickActionsActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "simulator/SimulatorHomeKeyInput.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

enum class SmokeStep : uint8_t {
  Start,
  Home,
  FileBrowser,
  FileBrowserSettings,
  RecentBooks,
  Settings,
  ReaderOptions,
  ReaderMenu,
  Sleep,
  Reader,
  ReaderInput,
  Done,
};

class SimulatorSmokeTest {
 public:
  void tick() {
    if (!enabled()) return;

    try {
      tickImpl();
    } catch (const std::exception& e) {
      fail("Unhandled exception: %s", e.what());
    } catch (...) {
      fail("Unhandled non-standard exception");
    }
  }

 private:
  enum class ScriptActionType : uint8_t {
    Press,
    Release,
    HomeTap,
    HomeLongPress,
    AssertTouchscreenDisabled,
    AssertTouchscreenEnabled,
    OpenSmokeBook,
    DisableReaderTouch,
    EnableReaderTouch,
    TouchDown,
    TouchMove,
    TouchRelease,
    AssertActivity,
    Render
  };

  struct ScriptAction {
    ScriptActionType type;
    MappedInputManager::Button button;
    const char* label;
    int settleFrames;
    int x;
    int y;
  };

  SmokeStep step = SmokeStep::Start;
  int settleFrames = 0;
  const char* activeStepName = nullptr;
  std::vector<ScriptAction> inputScript;
  size_t scriptIndex = 0;
  SmokeStep inputCompletionStep = SmokeStep::Done;

  static bool enabled() { return std::getenv("CROSSINK_SIMULATOR_SMOKE_TEST") != nullptr; }

  static int pageTurnCount() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS");
    if (raw == nullptr || raw[0] == '\0') {
      return 2;
    }
    return std::max(0, std::atoi(raw));
  }

  static bool landscapeReaderRequested() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_LANDSCAPE_READER");
    return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
  }

  static void applyRequestedTheme() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME");
    if (raw == nullptr || raw[0] == '\0') {
      return;
    }

    const int theme = std::atoi(raw);
    if (theme < 0 || theme >= CrossPointSettings::UI_THEME_COUNT) {
      fail("Invalid smoke test theme index: %d", theme);
    }

    SETTINGS.uiTheme = static_cast<uint8_t>(theme);
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Using theme index %d", theme);
  }

  static void verifyUpDownShortcutAvailability() {
    const auto allSettings = getSettingsList();
    const auto sideButtonSettings = buildControlsSideButtonSettingsList(allSettings);
    const bool hasSideButtonChord =
        std::any_of(sideButtonSettings.begin(), sideButtonSettings.end(),
                    [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SIDE_BUTTON_CHORD; });
    if (hasSideButtonChord != gpio.hasTouch()) {
      fail("Side-button chord availability does not match touch capability");
    }

    if (QuickActionsActivityTest::isTriggerAvailable(QuickActions::Trigger::UpDown) != gpio.hasTouch()) {
      fail("Quick Actions Up + Down availability does not match touch capability");
    }
  }

  [[noreturn]] static void fail(const char* message) {
    LOG_ERR("SMOKE", "%s", message);
    std::_Exit(2);
  }

  template <typename... Args>
  [[noreturn]] static void fail(const char* format, Args... args) {
    logPrintf("ERR", "SMOKE", format, args...);
    logPrintf("ERR", "SMOKE", "\n");
    std::_Exit(2);
  }

  static void renderCurrentStep(const char* name) {
    LOG_INF("SMOKE", "Rendering %s", name);
    if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail("Render was rejected for %s", name);
    }
  }

  void queueStep(const char* name, SmokeStep nextStep, int framesToSettle = 3) {
    activeStepName = name;
    settleFrames = framesToSettle;
    step = nextStep;
  }

  void tickImpl() {
    mappedInputManager.simulatorClearInputFrame();

    if (settleFrames > 0) {
      --settleFrames;
      if (settleFrames == 0 && activeStepName != nullptr) {
        renderCurrentStep(activeStepName);
        activeStepName = nullptr;
      }
      return;
    }

    switch (step) {
      case SmokeStep::Start:
        LOG_INF("SMOKE", "Starting simulator smoke test");
        if (!CrossPointSettings::verifySleepTimeoutMigrationContract()) {
          fail("Sleep timeout migration contract failed");
        }
        if (!CrossPointSettings::verifySleepScreenMigrationContract()) {
          fail("Sleep screen migration contract failed");
        }
        if (!SimulatorHomeKeyInput::verifyTimingContract()) {
          fail("Simulator Home key timing contract failed");
        }
        verifyUpDownShortcutAvailability();
        applyRequestedTheme();
        activityManager.goHome();
        queueStep("Home", SmokeStep::Home);
        break;

      case SmokeStep::Home:
        activityManager.goToFileBrowser("/books");
        queueStep("File Browser", SmokeStep::FileBrowser);
        break;

      case SmokeStep::FileBrowser:
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouchHardware()) {
          buildFileBrowserInputScript();
          step = SmokeStep::ReaderInput;
          break;
        }
#endif
        activityManager.goToRecentBooks();
        queueStep("Recent Books", SmokeStep::RecentBooks);
        break;

      case SmokeStep::FileBrowserSettings:
        activityManager.goToRecentBooks();
        queueStep("Recent Books", SmokeStep::RecentBooks);
        break;

      case SmokeStep::RecentBooks:
        if (mappedInputManager.hasHomeKey()) {
          renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
        }
        activityManager.goToSettings();
        queueStep(mappedInputManager.hasHomeKey() ? "Settings landscape" : "Settings", SmokeStep::Settings);
        break;

      case SmokeStep::Settings:
        renderer.setOrientation(GfxRenderer::Orientation::Portrait);
        activityManager.replaceActivity(std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager));
        queueStep("Reader Options", SmokeStep::ReaderOptions);
        break;

      case SmokeStep::ReaderOptions:
        activityManager.replaceActivity(
            std::make_unique<EpubReaderMenuActivity>(renderer, mappedInputManager, "Smoke Test", 1, 1, 0,
                                                     SETTINGS.orientation, false, false, false, false, false, false));
        queueStep("Reader Menu", SmokeStep::ReaderMenu);
        break;

      case SmokeStep::ReaderMenu:
        activityManager.goToSleep();
        queueStep("Sleep", SmokeStep::Sleep);
        break;

      case SmokeStep::Sleep: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') {
          LOG_INF("SMOKE", "Skipping Reader step; CROSSINK_SIMULATOR_SMOKE_BOOK is not set");
          step = SmokeStep::Reader;
          break;
        }
        if (!Storage.exists(bookPath)) {
          fail("Smoke test book is missing: %s", bookPath);
        }
        if (landscapeReaderRequested()) {
          SETTINGS.orientation = CrossPointSettings::LANDSCAPE_CCW;
          LOG_INF("SMOKE", "Opening smoke reader in landscape");
        }
        activityManager.goToReader(bookPath, true);
        queueStep("Reader", SmokeStep::Reader, 8);
        break;
      }

      case SmokeStep::Reader:
        buildReaderInputScript();
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::ReaderInput:
        runReaderInputScript();
        break;

      case SmokeStep::Done:
        LOG_INF("SMOKE", "Simulator smoke test passed");
        std::_Exit(0);
    }
  }

  static ScriptAction press(MappedInputManager::Button button) {
    return {ScriptActionType::Press, button, nullptr, 0, 0, 0};
  }

  static ScriptAction release(MappedInputManager::Button button) {
    return {ScriptActionType::Release, button, nullptr, 0, 0, 0};
  }

  static ScriptAction homeTap() {
    return {ScriptActionType::HomeTap, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction homeLongPress() {
    return {ScriptActionType::HomeLongPress, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenDisabled() {
    return {ScriptActionType::AssertTouchscreenDisabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenEnabled() {
    return {ScriptActionType::AssertTouchscreenEnabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction openSmokeBook() {
    return {ScriptActionType::OpenSmokeBook, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction disableReaderTouch() {
    return {ScriptActionType::DisableReaderTouch, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction enableReaderTouch() {
    return {ScriptActionType::EnableReaderTouch, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction render(const char* label, int framesToSettle = 3) {
    return {ScriptActionType::Render, MappedInputManager::Button::Back, label, framesToSettle, 0, 0};
  }

#if CROSSINK_APP_CAP_TOUCH
  static ScriptAction touchDown(const int x, const int y) {
    return {ScriptActionType::TouchDown, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction touchMove(const int x, const int y) {
    return {ScriptActionType::TouchMove, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction touchRelease(const int x, const int y) {
    return {ScriptActionType::TouchRelease, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction assertActivity(const char* name) {
    return {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, name, 0, 0, 0};
  }
#endif

  void addTap(MappedInputManager::Button button) {
    inputScript.push_back(press(button));
    inputScript.push_back(release(button));
  }

  void buildReaderInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::Done;

    const int turns = pageTurnCount();
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouch()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      if (width <= 0 || height <= 0) fail("Touch smoke test has invalid screen dimensions");
      LOG_INF("SMOKE", "Running touch reader input script with %d page turn(s)", turns);
      for (int i = 0; i < turns; ++i) {
        inputScript.push_back(touchDown(width * 5 / 6, height / 2));
        inputScript.push_back(touchRelease(width * 5 / 6, height / 2));
        inputScript.push_back(render("Reader after touch page forward", 4));
      }
      if (mappedInputManager.hasHomeKey()) {
        // X4 Pro reserves the top-edge swipe for its frontlight overlay and
        // moves the reader menu to the bottom edge.
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel opened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
        inputScript.push_back(touchDown(header.x + header.width - 32, header.y + header.height / 2));
        inputScript.push_back(touchRelease(header.x + header.width - 32, header.y + header.height / 2));
        inputScript.push_back(render("Home opened by Frontlight Panel Home button", 4));
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Reader reopened after Frontlight Panel Home button", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened after Home button", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(20, height / 3));
        inputScript.push_back(touchMove(20, 8));
        inputScript.push_back(touchRelease(20, 8));
        inputScript.push_back(render("Frontlight Panel remains open after in-drawer swipe up", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        // X4 Pro's portrait frontlight sheet ends just below mid-screen; this
        // point lands in its centered 29 px handle band.
        inputScript.push_back(touchDown(width / 2, height * 21 / 40));
        inputScript.push_back(touchMove(width / 2, 8));
        inputScript.push_back(touchRelease(width / 2, 8));
        inputScript.push_back(render("Reader restored after Frontlight Panel handle drag up", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        // The fourth action-bar slot opens Global Settings through the real
        // FrontlightPanelActivity callback path.
        inputScript.push_back(touchDown(width * 7 / 10, height * 15 / 32));
        inputScript.push_back(touchRelease(width * 7 / 10, height * 15 / 32));
        inputScript.push_back(render("Global Settings opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("Settings"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height / 2));
        inputScript.push_back(touchRelease(width / 2, height / 2));
        inputScript.push_back(render("Global Settings remains open after interior swipe up", 4));
        inputScript.push_back(assertActivity("Settings"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader restored after Settings bottom-edge swipe", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened after Global Settings", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width * 3 / 10, height * 3 / 8));
        inputScript.push_back(touchRelease(width * 3 / 10, height * 3 / 8));
        inputScript.push_back(render("Sync dialog opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height - 60));
        inputScript.push_back(touchRelease(width / 2, height - 60));
        inputScript.push_back(render("Reader restored after dismissing Frontlight sync dialog", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height / 2 + 24));
        inputScript.push_back(touchRelease(width / 2, height / 2 + 24));
        inputScript.push_back(render("Reader Font opened from touch reader menu", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader Menu root restored by simulated Home key tap", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader restored by simulated Home key tap at drawer root", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu reopened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height / 2 - 14));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader restored after Reader Menu handle drag down", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(disableReaderTouch());
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from Home key hold with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Reader restored after Home key menu with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Home opened from simulated Home key tap", 4));
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(enableReaderTouch());
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Reader reopened after simulated Home key tap", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      } else {
        // Sticky uses the same vertical gesture split as X4 Pro: swipe down
        // opens reader details/actions and swipe up opens the bottom menu.
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Sticky Reader Details opened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(20, height / 3));
        inputScript.push_back(touchMove(20, 8));
        inputScript.push_back(touchRelease(20, 8));
        inputScript.push_back(render("Sticky Reader Details remains open after in-drawer swipe up", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader restored after Sticky details outside tap", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      }
      inputScript.push_back(render("Reader Menu opened from touch gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));

      // Touch every bottom-drawer tab slot, then dismiss from its handle.
      const int tabY = height - 28;
      for (int tab = 0; tab < static_cast<int>(READER_DRAWER_TAB_COUNT); ++tab) {
        const int tabX = width * (tab * 2 + 1) / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2);
        inputScript.push_back(touchDown(tabX, tabY));
        inputScript.push_back(touchRelease(tabX, tabY));
        inputScript.push_back(render("Touch Reader Menu tab", 3));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      }

      const int moreTabX = width / 2;
      const int drawerTop = height / 2;
      constexpr int rootRowStep = 60;
      constexpr int rootRowCenterOffset = 31;
      inputScript.push_back(touchDown(moreTabX, tabY));
      inputScript.push_back(touchRelease(moreTabX, tabY));
      inputScript.push_back(render("Touch Reader Menu More tab", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Go to Percent pane", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Auto Page Turn pane", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height - 8));
      inputScript.push_back(touchRelease(width / 2, height - 8));
      inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(width / 2, drawerTop - 14));
      inputScript.push_back(touchRelease(width / 2, drawerTop - 14));
      inputScript.push_back(render("Reader restored after drawer handle tap", 4));
      inputScript.push_back(assertActivity("EpubReader"));

      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Reader Menu reopened for bottom-edge Home gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height / 2 + 8));
      inputScript.push_back(touchRelease(width / 2, height / 2 + 8));
      inputScript.push_back(render("Reader Menu remains open after interior swipe up", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Home opened from Reader Menu bottom-edge swipe", 6));
      inputScript.push_back(assertActivity("Home"));
      return;
    }
#endif
    for (int i = 0; i < turns; i++) {
      addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Reader after page forward", 4));
    }

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu opened from EPUB", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Menu Reader Options selection", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options opened from Reader Menu", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Options after navigation", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options after toggle", 3));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Menu after closing Reader Options", 4));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after closing Reader Menu", 4));

    LOG_INF("SMOKE", "Running reader input script with %d page turn(s)", turns);
  }

#if CROSSINK_APP_CAP_TOUCH
  void buildFileBrowserInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::FileBrowserSettings;

    if (mappedInputManager.hasHomeKey()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight Panel opened outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width * 9 / 10, height / 2));
      inputScript.push_back(touchRelease(width * 9 / 10, height / 2));
      inputScript.push_back(render("Reader touchscreen disabled from Frontlight Panel outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width / 2, height - 60));
      inputScript.push_back(touchRelease(width / 2, height - 60));
      inputScript.push_back(render("File Browser restored after disabling reader touchscreen", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(assertTouchscreenDisabled());
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight Panel reopened outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width * 9 / 10, height / 2));
      inputScript.push_back(touchRelease(width * 9 / 10, height / 2));
      inputScript.push_back(render("Reader touchscreen enabled from Frontlight Panel outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width / 2, height - 60));
      inputScript.push_back(touchRelease(width / 2, height - 60));
      inputScript.push_back(render("File Browser restored after enabling reader touchscreen", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(assertTouchscreenEnabled());
    }

    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
    const auto backLayout = TouchHeaderBackButton::layout(header);
    const int x = header.x + header.width - backLayout.iconRect.width / 2;
    const int y = backLayout.iconRect.y + backLayout.iconRect.height / 2;
    inputScript.push_back(touchDown(x, y));
    inputScript.push_back(touchRelease(x, y));
    inputScript.push_back(render("File Browser Settings opened from header shortcut", 4));
    inputScript.push_back(assertActivity("FileBrowserSettings"));
    const int rowY = header.y + header.height + 32;
    inputScript.push_back(touchDown(renderer.getScreenWidth() / 2, rowY));
    inputScript.push_back(touchRelease(renderer.getScreenWidth() / 2, rowY));
    inputScript.push_back(render("File Browser Settings toggle without row highlight", 4));
    inputScript.push_back(assertActivity("FileBrowserSettings"));
  }
#endif

  void runReaderInputScript() {
    if (scriptIndex >= inputScript.size()) {
      step = inputCompletionStep;
      return;
    }

    const auto& action = inputScript[scriptIndex++];
    switch (action.type) {
      case ScriptActionType::Press:
        mappedInputManager.simulatorInjectPress(action.button);
        break;
      case ScriptActionType::Release:
        mappedInputManager.simulatorInjectRelease(action.button);
        break;
      case ScriptActionType::HomeTap:
        simulatorHomeKeyInput.injectTap();
        break;
      case ScriptActionType::HomeLongPress:
        simulatorHomeKeyInput.injectLongPress();
        break;
      case ScriptActionType::AssertTouchscreenDisabled:
        if (!SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be disabled");
        break;
      case ScriptActionType::AssertTouchscreenEnabled:
        if (SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be enabled");
        break;
      case ScriptActionType::OpenSmokeBook: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') fail("Smoke test book path is missing");
        activityManager.goToReader(bookPath, true);
        break;
      }
      case ScriptActionType::DisableReaderTouch:
        SETTINGS.disableReaderTouchscreen = true;
        break;
      case ScriptActionType::EnableReaderTouch:
        SETTINGS.disableReaderTouchscreen = false;
        break;
      case ScriptActionType::TouchDown:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchDown(action.x, action.y);
#endif
        break;
      case ScriptActionType::TouchMove:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchMove(action.x, action.y);
#endif
        break;
      case ScriptActionType::TouchRelease:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchRelease(action.x, action.y);
#endif
        break;
      case ScriptActionType::AssertActivity:
        if (!activityManager.isCurrentActivityNamed(action.label)) fail("Expected current activity: %s", action.label);
        break;
      case ScriptActionType::Render:
        queueStep(action.label, SmokeStep::ReaderInput, action.settleFrames);
        break;
    }
  }
};

SimulatorSmokeTest smokeTest;

}  // namespace

void runSimulatorSmokeTestTick() { smokeTest.tick(); }

#endif
