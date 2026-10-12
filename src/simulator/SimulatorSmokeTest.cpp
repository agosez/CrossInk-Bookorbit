#ifdef SIMULATOR

#include "SimulatorSmokeTest.h"

#include <BookOrbitCredentialStore.h>
#include <Epub.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include "CrossPointState.h"
#if CROSSINK_SCALABLE_FONTS
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <HalScalableFont.h>

#include <fstream>

#include "FontInstaller.h"
#include "TtfRenderProfileStore.h"
#include "util/WordSelectNavigator.h"
#endif
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "DeviceCapabilities.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/library/LibraryActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderDrawerActivity.h"
#include "activities/reader/ReaderFontLoading.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/SideButtonShortcuts.h"
#include "activities/settings/QuickActionsActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/FrontlightPanelActivity.h"
#include "components/HeaderDate.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "simulator/SimulatorHomeKeyInput.h"
#include "util/ButtonShortcutController.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

enum class SmokeStep : uint8_t {
  Start,
  HomeReaderReader,
  HomeReaderNested,
  HomeReaderConfirmation,
  HomeReaderTrigger,
  HomeReaderUnwound,
  HomeReaderReturnedHome,
  HomeReaderNonReaderParent,
  HomeReaderNonReaderNested,
  HomeReaderNonReaderHome,
  BackHomeReader,
  BackHomeNested,
  BackHomeReturnedReader,
  BackHomeReturnedHome,
  Home,
  FileBrowser,
  FileBrowserSettings,
  Library,
  RecentLibrary,
  Settings,
  SideButtons,
  ReaderMenu,
  Sleep,
  Reader,
  ReaderInput,
  CarouselHome,
  FrontlightLayout,
  FrontlightLayoutRendered,
  ThemeHome,
  ThemeSettings,
  ThemeReturned,
  ThemeFresh,
  StatusBarEditor,
  StatusBarPicker,
  Done,
};

class HomeReaderSmokeActivity final : public Activity {
 public:
  HomeReaderSmokeActivity(const char* activityName, const bool reader, GfxRenderer& renderer,
                          MappedInputManager& mappedInput, const bool bookReader = false)
      : Activity(activityName, renderer, mappedInput), reader(reader), bookReader(bookReader) {}

  bool isReaderActivity() const override { return reader; }
  bool isBookReaderActivity() const override { return bookReader; }

 private:
  bool reader;
  bool bookReader;
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
    ConfigureHomeButtonPowerLock,
    WaitForPowerLongPress,
    AssertHomeButtonDisabled,
    AssertHomeButtonEnabled,
    AssertTouchscreenDisabled,
    AssertTouchscreenEnabled,
    AssertTtfProfileNative,
    OpenSmokeBook,
    OpenFrontlightSettings,
    ClearFrontlightSyncBook,
    PrepareFrontlightSync,
    OpenFrontlightSync,
    CheckFrontlightSync,
    DisableReadingStats,
    EnableReadingStats,
    DisableReaderTouch,
    EnableReaderTouch,
    TouchDown,
    TouchMove,
    TouchRelease,
    AssertReaderMenu,
    AssertSettingsNavigation,
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
  unsigned libraryRefreshPass = 0;
  uint16_t libraryBaselineBooks = 0;
  SmokeStep inputCompletionStep = SmokeStep::Done;
  unsigned carouselCachePass = 0;
  std::filesystem::file_time_type carouselCacheWrittenAt;
  std::filesystem::file_time_type carouselSecondWrittenAt;
  uint64_t carouselCacheHash = 0;
  uint64_t carouselScreenHash = 0;
  unsigned frontlightLayoutPass = 0;
  unsigned homeThemePass = 0;
  uint64_t homeThemeScreenHash = 0;
  std::string homeThemeBookPath;

  uint8_t homeReaderReaderKind = 0;
  uint8_t homeReaderCancelledMask = 0;
  Activity* homeReaderSmokeReader = nullptr;
  bool homeReaderConfirmationAccepted = false;
  bool backHomeChildCancelled = false;

  void prepareRecentLibrary() {
    SETTINGS.librarySortMethod = 4;
    SETTINGS.librarySortDescending = 1;
    SETTINGS.libraryUseMetadata = 1;
    SETTINGS.libraryShowTxt = 1;
    SETTINGS.libraryHideFinishedBooks = 0;
    for (int i = 0; i < 20; ++i) {
      const std::string path = "/books/recent-smoke-" + std::to_string(i) + ".txt";
      if (!Storage.writeFile(path.c_str(), "Recent Library smoke fixture")) fail("Cannot create recent fixture");
      RECENT_BOOKS.addOrUpdateBook(path, "Title " + std::to_string(i), "Author " + std::to_string(i), "");
    }
    Storage.remove(library::libraryIndexPath());
    library::invalidateLibraryIndex();
  }

  static const char* homeReaderSmokeReaderName(const uint8_t kind) {
    switch (kind) {
      case 0:
        return "EpubReader";
      case 1:
        return "TxtReader";
      default:
        return "XtcReader";
    }
  }

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

  static void verifyMixedPageGestures() {
#if CROSSINK_APP_CAP_TOUCH
    if (!gpio.hasTouch()) return;
    const uint8_t savedNext = SETTINGS.pageTurnGesture;
    const uint8_t savedPrevious = SETTINGS.previousPageGesture;
    const int width = renderer.getScreenWidth();
    const int y = renderer.getScreenHeight() / 2;
    mappedInputManager.setReaderMode(true);
    for (uint8_t next = 0; next < CrossPointSettings::PAGE_TURN_GESTURE_COUNT; ++next) {
      for (uint8_t previous = 0; previous < CrossPointSettings::PAGE_TURN_GESTURE_COUNT; ++previous) {
        SETTINGS.pageTurnGesture = next;
        SETTINGS.previousPageGesture = previous;
        const bool inverted = next == CrossPointSettings::INVERTED_TAP || previous == CrossPointSettings::INVERTED_TAP;
        const bool nextTap = next == CrossPointSettings::TAP_AND_SWIPE || next == CrossPointSettings::TAP_ONLY ||
                             next == CrossPointSettings::INVERTED_TAP;
        const bool previousTap = previous == CrossPointSettings::TAP_AND_SWIPE ||
                                 previous == CrossPointSettings::TAP_ONLY ||
                                 previous == CrossPointSettings::INVERTED_TAP;
        for (const int x : {0, width / 3 - 1, width / 3, width * 2 / 3 - 1, width * 2 / 3, width - 1}) {
          mappedInputManager.simulatorInjectTouchDown(x, y);
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          const auto result = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
          const bool nextZone = inverted ? x < width * 2 / 3 : x >= width / 3;
          const bool expectedNext = nextTap && (!previousTap || nextZone);
          const bool expectedPrevious = previousTap && (!nextTap || !nextZone);
          if (!result.tapped || result.next != expectedNext || result.prev != expectedPrevious) {
            fail("Mixed page tap mismatch: next=%u previous=%u x=%d", next, previous, x);
          }
          mappedInputManager.simulatorClearInputFrame();
        }
        for (const bool right : {false, true}) {
          const int startX = right ? 1 : width - 2;
          const int endX = right ? width - 2 : 1;
          mappedInputManager.simulatorInjectTouchDown(startX, y);
          mappedInputManager.simulatorInjectTouchMove(endX, y);
          mappedInputManager.simulatorInjectTouchRelease(endX, y);
          const auto result = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
          const uint8_t mode = right ? previous : next;
          const bool expected = (mode == CrossPointSettings::TAP_AND_SWIPE || mode == CrossPointSettings::SWIPE_ONLY);
          if (result.next != (!right && expected) || result.prev != (right && expected) ||
              (right && !expected && mappedInputManager.wasReleased(MappedInputManager::Button::Back))) {
            fail("Mixed page swipe mismatch: next=%u previous=%u right=%d", next, previous, right);
          }
          mappedInputManager.simulatorClearInputFrame();
        }
      }
    }
    SETTINGS.pageTurnGesture = savedNext;
    SETTINGS.previousPageGesture = savedPrevious;
    mappedInputManager.setReaderMode(false);
    LOG_INF("SMOKE", "All 25 mixed page gesture combinations passed");
#endif
  }

  static void verifyStatusBarSettings() {
    JsonDocument original;
    SETTINGS.toJson(original);
    const HalClock originalClock = halClock;
    halClock = HalClock{};  // Simulate a failed RTC probe during settings loading.
    for (const int clock : {0, 1}) {
      JsonDocument legacy;
      legacy.set(original);
      legacy.remove("displayStatusBar");
      legacy["showClockOutsideReader"] = clock;
      SETTINGS.fromJson(legacy.as<JsonVariantConst>());
      if (SETTINGS.displayStatusBar.slots[1] != (clock ? ReaderStatusBarItem::Clock : ReaderStatusBarItem::Empty) ||
          SETTINGS.displayStatusBar.slots[2] != ReaderStatusBarItem::Battery)
        fail("Display clock migration failed");
    }
    SETTINGS.displayStatusBar.slots = {ReaderStatusBarItem::Date, ReaderStatusBarItem::Clock,
                                       ReaderStatusBarItem::Empty};
    SETTINGS.topReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Clock;
    SETTINGS.bottomReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Date;
    JsonDocument saved;
    SETTINGS.toJson(saved);
    if (!saved["showClockOutsideReader"].isNull()) fail("Obsolete clock setting was saved");
    SETTINGS.displayStatusBar = DisplayStatusBarConfig{};
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.displayStatusBar.slots[0] != ReaderStatusBarItem::Date ||
        SETTINGS.displayStatusBar.slots[1] != ReaderStatusBarItem::Clock ||
        SETTINGS.displayStatusBar.slots[2] != ReaderStatusBarItem::Empty)
      fail("Display slots did not survive reload");
    if (SETTINGS.topReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] != ReaderStatusBarItem::Clock ||
        SETTINGS.bottomReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] != ReaderStatusBarItem::Date)
      fail("Reader clock/date slots did not survive an unavailable RTC");
    halClock = originalClock;
    const auto display = buildGroupedDisplaySettingsList(getSettingsList());
    if (std::none_of(display.begin(), display.end(),
                     [](const auto& item) { return item.action == SettingAction::DisplayStatusBar; }))
      fail("Display status bar setting is missing");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    LOG_INF("SMOKE", "Display status bar migration and persistence passed");
  }

  static void captureStatusBarScreen(const char* name) {
    const char* output = std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES");
    if (!output) return;
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output) / (std::string(name) + ".pgm");
    FILE* image = std::fopen(path.c_str(), "wb");
    if (!image) fail("Cannot create status bar capture");
    const int width = renderer.getScreenWidth();
    const int height = renderer.getScreenHeight();
    std::fprintf(image, "P5\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
    }
    std::fclose(image);
  }

  static void verifyReaderControlsSettings() {
    const auto& base = getBaseSettingsList();
    if (base.size() > BASE_SETTINGS_CAPACITY || base.capacity() < BASE_SETTINGS_CAPACITY) {
      fail("Base settings allocation mismatch: size=%zu capacity=%zu", base.size(), base.capacity());
    }
    const auto all = getSettingsList();
    const auto gestures = buildControlsTapsGesturesSettingsList(all);
    if (gpio.hasTouch()) {
      if (gestures.size() < 3 || gestures[0].nameId != StrId::STR_NEXT_PAGE ||
          gestures[1].nameId != StrId::STR_PREV_PAGE || gestures[0].enumValues != gestures[1].enumValues) {
        fail("Page gesture settings order/options mismatch");
      }
      if (gpio.supportsMultiTouch() && (gestures.size() < 4 || gestures[3].nameId != StrId::STR_TWO_FINGER_ROTATION)) {
        fail("Two-finger rotation gesture setting order mismatch");
      }
      const size_t statusIndex = gpio.supportsMultiTouch() ? 4 : 2;
      if (gestures.size() <= statusIndex || gestures[statusIndex].nameId != StrId::STR_TAP_HIDE_STATUS_BAR) {
        fail("Status bar gesture setting order mismatch");
      }
    } else if (!gestures.empty()) {
      fail("Touch gestures exposed on a button-only device");
    }
    const auto device = buildSystemDeviceSettingsList(all);
    if (device.size() < 3 || device[1].nameId != StrId::STR_TIME_TO_SLEEP ||
        device[2].nameId != StrId::STR_CUSTOM_BOOTSCREEN) {
      fail("Custom bootscreen setting order mismatch");
    }
    JsonDocument original;
    SETTINGS.toJson(original);
    for (uint8_t mode = 0; mode <= CrossPointSettings::PAGE_TURN_GESTURE_DISABLED; ++mode) {
      JsonDocument legacy;
      legacy["pageTurnGesture"] = mode;
      SETTINGS.fromJson(legacy.as<JsonVariantConst>());
      if (SETTINGS.pageTurnGesture != mode || SETTINGS.previousPageGesture != mode) {
        fail("Legacy page gesture migration mismatch");
      }
    }
    constexpr uint8_t importedGestures[] = {CrossPointSettings::TAP_AND_SWIPE, CrossPointSettings::TAP_ONLY,
                                            CrossPointSettings::SWIPE_ONLY, CrossPointSettings::INVERTED_TAP};
    for (uint8_t mode = 0; mode < 4; ++mode) {
      JsonDocument crosspoint;
      crosspoint["touchReaderControls"] = mode;
      crosspoint["disableReaderTouchscreen"] = 1;
      SETTINGS.fromJson(crosspoint.as<JsonVariantConst>(), true);
      if (SETTINGS.disableReaderTouchscreen || SETTINGS.touchReaderControls != (mode != 0) ||
          SETTINGS.pageTurnGesture != importedGestures[mode] ||
          SETTINGS.previousPageGesture != importedGestures[mode]) {
        fail("CrossPoint touch settings migration mismatch");
      }
      JsonDocument migrated;
      SETTINGS.toJson(migrated);
      SETTINGS.disableReaderTouchscreen = 1;
      SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
      SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
      SETTINGS.fromJson(migrated.as<JsonVariantConst>());
      if (SETTINGS.disableReaderTouchscreen || SETTINGS.touchReaderControls != (mode != 0) ||
          SETTINGS.pageTurnGesture != importedGestures[mode] ||
          SETTINGS.previousPageGesture != importedGestures[mode]) {
        fail("Migrated CrossPoint touch settings did not survive reload");
      }
    }
    // The namespaced file must preserve intentional locks, including older files without gesture keys.
    JsonDocument locked;
    locked["touchReaderControls"] = 1;
    locked["disableReaderTouchscreen"] = 1;
    SETTINGS.fromJson(locked.as<JsonVariantConst>());
    if (!SETTINGS.disableReaderTouchscreen) fail("CrossInk touch lock was lost");
    locked["pageTurnGesture"] = CrossPointSettings::TAP_ONLY;
    locked["previousPageGesture"] = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.fromJson(locked.as<JsonVariantConst>(), true);
    if (!SETTINGS.disableReaderTouchscreen || SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::PAGE_TURN_GESTURE_DISABLED) {
      fail("Legacy CrossInk gesture settings were treated as CrossPoint");
    }
    SETTINGS.previousPageGesture = CrossPointSettings::SWIPE_ONLY;
    SETTINGS.pageTurnGesture = CrossPointSettings::TAP_ONLY;
    SETTINGS.customBootscreenEnabled = 0;
    SETTINGS.tapToHideStatusBar = 0;
    JsonDocument saved;
    SETTINGS.toJson(saved);
    SETTINGS.previousPageGesture = CrossPointSettings::TAP_AND_SWIPE;
    SETTINGS.customBootscreenEnabled = 1;
    SETTINGS.tapToHideStatusBar = 1;
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY || SETTINGS.customBootscreenEnabled ||
        SETTINGS.tapToHideStatusBar) {
      fail("Reader controls settings round-trip mismatch");
    }
    constexpr char CROSSINK_SETTINGS_FILE_BAK[] = "/.crosspoint/crossink-settings.json.bak";
    constexpr char LEGACY_SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
    const char* const crossInkSettingsPath = CrossPointSettings::getFilePath();
    const bool hadCrossInkSettings = Storage.exists(crossInkSettingsPath);
    const String savedCrossInkSettings = hadCrossInkSettings ? Storage.readFile(crossInkSettingsPath) : String();
    const bool hadCrossInkSettingsBackup = Storage.exists(CROSSINK_SETTINGS_FILE_BAK);
    const String savedCrossInkSettingsBackup =
        hadCrossInkSettingsBackup ? Storage.readFile(CROSSINK_SETTINGS_FILE_BAK) : String();
    const bool hadLegacySettings = Storage.exists(LEGACY_SETTINGS_FILE_JSON);
    const String savedLegacySettings = hadLegacySettings ? Storage.readFile(LEGACY_SETTINGS_FILE_JSON) : String();

    JsonDocument crossInkSettings;
    crossInkSettings["touchReaderControls"] = CrossPointSettings::TOUCH_READER_ON;
    crossInkSettings["pageTurnGesture"] = CrossPointSettings::TAP_ONLY;
    crossInkSettings["previousPageGesture"] = CrossPointSettings::SWIPE_ONLY;
    crossInkSettings["disableReaderTouchscreen"] = 0;
    String crossInkJson;
    serializeJson(crossInkSettings, crossInkJson);

    JsonDocument crossPointSettings;
    crossPointSettings["touchReaderControls"] = 2;
    crossPointSettings["disableReaderTouchscreen"] = 1;
    String crossPointJson;
    serializeJson(crossPointSettings, crossPointJson);

    if (!Storage.writeFile(crossInkSettingsPath, crossInkJson) ||
        !Storage.writeFile(LEGACY_SETTINGS_FILE_JSON, crossPointJson)) {
      fail("Could not write settings migration test fixture");
    }
    SETTINGS.disableReaderTouchscreen = 1;
    SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    if (!SETTINGS.loadFromFile() || SETTINGS.disableReaderTouchscreen ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY) {
      fail("CrossInk settings file did not take precedence over CrossPoint settings");
    }

    // A corrupt CrossInk file still blocks the foreign fallback. It is safer
    // to leave settings unchanged than to silently import CrossPoint values.
    if (!Storage.writeFile(crossInkSettingsPath, "{")) fail("Could not corrupt CrossInk settings test fixture");
    if (SETTINGS.loadFromFile() || SETTINGS.disableReaderTouchscreen ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY) {
      fail("Corrupt CrossInk settings fell through to CrossPoint settings");
    }

    // An interrupted atomic replacement leaves the CrossInk backup as the
    // sole namespaced file. Recover it before considering CrossPoint's file.
    if (!Storage.writeFile(CROSSINK_SETTINGS_FILE_BAK, crossInkJson) || !Storage.remove(crossInkSettingsPath)) {
      fail("Could not create interrupted CrossInk settings fixture");
    }
    SETTINGS.disableReaderTouchscreen = 1;
    SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    if (!SETTINGS.loadFromFile() || SETTINGS.disableReaderTouchscreen ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY || !Storage.exists(crossInkSettingsPath) ||
        Storage.exists(CROSSINK_SETTINGS_FILE_BAK)) {
      fail("Interrupted CrossInk settings save did not recover before CrossPoint import");
    }

    if (hadCrossInkSettings) {
      if (!Storage.writeFile(crossInkSettingsPath, savedCrossInkSettings)) fail("Could not restore CrossInk settings");
    } else if (Storage.exists(crossInkSettingsPath) && !Storage.remove(crossInkSettingsPath)) {
      fail("Could not remove CrossInk settings test fixture");
    }
    if (hadCrossInkSettingsBackup) {
      if (!Storage.writeFile(CROSSINK_SETTINGS_FILE_BAK, savedCrossInkSettingsBackup)) {
        fail("Could not restore CrossInk settings backup");
      }
    } else if (Storage.exists(CROSSINK_SETTINGS_FILE_BAK) && !Storage.remove(CROSSINK_SETTINGS_FILE_BAK)) {
      fail("Could not remove CrossInk settings backup fixture");
    }
    if (hadLegacySettings) {
      if (!Storage.writeFile(LEGACY_SETTINGS_FILE_JSON, savedLegacySettings)) fail("Could not restore legacy settings");
    } else if (Storage.exists(LEGACY_SETTINGS_FILE_JSON) && !Storage.remove(LEGACY_SETTINGS_FILE_JSON)) {
      fail("Could not remove legacy settings test fixture");
    }

    SETTINGS.librarySortMethod = 3;
    SETTINGS.librarySortDescending = 0;
    SETTINGS.libraryListExpanded = 1;
    SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_GRID;
    SETTINGS.libraryShowMarkdown = 0;
    SETTINGS.libraryHideFinishedBooks = 1;
    JsonDocument librarySaved;
    SETTINGS.toJson(librarySaved);
    SETTINGS.librarySortMethod = 0;
    SETTINGS.librarySortDescending = 1;
    SETTINGS.libraryListExpanded = 0;
    SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_LIST;
    SETTINGS.libraryShowMarkdown = 1;
    SETTINGS.libraryHideFinishedBooks = 0;
    SETTINGS.fromJson(librarySaved.as<JsonVariantConst>());
    if (SETTINGS.librarySortMethod != 3 || SETTINGS.librarySortDescending || !SETTINGS.libraryListExpanded ||
        SETTINGS.recentBooksView != CrossPointSettings::RECENT_BOOKS_GRID || SETTINGS.libraryShowMarkdown ||
        !SETTINGS.libraryHideFinishedBooks) {
      fail("Library settings round-trip mismatch");
    }
    librarySaved["librarySortMethod"] = 99;
    librarySaved["libraryShowTxt"] = 2;
    librarySaved["libraryHideFinishedBooks"] = 2;
    librarySaved["recentBooksView"] = 2;
    SETTINGS.fromJson(librarySaved.as<JsonVariantConst>());
    if (SETTINGS.librarySortMethod != 3 || SETTINGS.libraryShowTxt != 1 || !SETTINGS.libraryHideFinishedBooks ||
        SETTINGS.recentBooksView != CrossPointSettings::RECENT_BOOKS_LIST) {
      fail("Invalid Library settings were not rejected");
    }
    librarySaved.remove("recentBooksView");
    SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_GRID;
    SETTINGS.fromJson(librarySaved.as<JsonVariantConst>());
    if (SETTINGS.recentBooksView != CrossPointSettings::RECENT_BOOKS_LIST)
      fail("Missing Recently Opened view did not default to List");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
  }

  static void verifyUpDownShortcutAvailability() {
    const auto allSettings = getSettingsList();
    const auto sideButtonSettings = buildControlsSideButtonSettingsList(allSettings);
    const bool hasSideButtonChord =
        std::any_of(sideButtonSettings.begin(), sideButtonSettings.end(),
                    [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SIDE_BUTTON_CHORD; });
    if (hasSideButtonChord != deviceSupportsSideButtonChord(gpio)) {
      fail("Side-button chord availability does not match device controls");
    }

    if (QuickActionsActivityTest::isTriggerAvailable(QuickActions::Trigger::UpDown) !=
        deviceSupportsSideButtonChord(gpio)) {
      fail("Quick Actions Up + Down availability does not match device controls");
    }

    const auto chordSetting = std::find_if(allSettings.begin(), allSettings.end(), [](const SettingInfo& setting) {
      return settingKeyIs(setting, "powerChordAction");
    });
    if (chordSetting == allSettings.end()) fail("Power chord setting is missing");
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_QUICK_ACTIONS) == chordSetting->enumRawValues.end()) {
      fail("Quick Actions is missing from the Power + Up chord setting");
    }
    // The counts are one past upstream's: this fork's BookOrbit Sync sits after its last action.
    if (CrossPointSettings::HOME_READER != 36 || CrossPointSettings::SHORT_PWRBTN_COUNT != 38 ||
        CrossPointSettings::CHORD_HOME_READER != 32 || CrossPointSettings::POWER_CHORD_ACTION_COUNT != 34) {
      fail("Home/Reader changed persisted shortcut IDs or counts");
    }
    if (CrossPointSettings::BOOKORBIT_SYNC != 37 || CrossPointSettings::CHORD_BOOKORBIT_SYNC != 33 ||
        CrossPointSettings::LONG_MENU_BOOKORBIT_SYNC != 25) {
      fail("BookOrbit Sync changed persisted shortcut IDs without a migration");
    }
    if (QuickActions::actionLabel(CrossPointSettings::HOME_READER) != StrId::STR_HOME_READER ||
        std::string(I18N.get(StrId::STR_HOME_READER)) != "Home/Reader") {
      fail("Home/Reader shortcut label mismatch");
    }
    if (QuickActions::isActionAvailable(CrossPointSettings::HOME_READER) != !gpio.hasTouch()) {
      fail("Home/Reader capability gating does not match button-only devices");
    }
    const auto containsShortcut = [](const std::vector<SettingInfo>& settings, const char* key,
                                     const ShortcutOptionCatalog catalog) {
      const auto setting = std::find_if(settings.begin(), settings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == settings.end()) return false;
      const uint8_t raw = shortcutRawValue(catalog, CrossPointSettings::HOME_READER);
      const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), raw);
      return choice != setting->enumRawValues.end() &&
             setting->enumValues[static_cast<size_t>(choice - setting->enumRawValues.begin())] ==
                 StrId::STR_HOME_READER;
    };
    const bool shouldExposeHomeReader = !gpio.hasTouch();
    if (containsShortcut(allSettings, "shortPwrBtn", ShortcutOptionCatalog::PowerButton) != shouldExposeHomeReader ||
        containsShortcut(sideButtonSettings, "sideButtonUpShort", ShortcutOptionCatalog::SideButton) !=
            shouldExposeHomeReader ||
        containsShortcut(allSettings, "powerChordAction", ShortcutOptionCatalog::ButtonChord) !=
            shouldExposeHomeReader) {
      fail("Home/Reader shortcut availability or settings mapping mismatch");
    }
    if (shortcutRawValue(ShortcutOptionCatalog::HomeButton, CrossPointSettings::HOME_READER) !=
            SHORTCUT_OPTION_UNAVAILABLE ||
        shortcutRawValue(ShortcutOptionCatalog::LongPress, CrossPointSettings::HOME_READER) !=
            SHORTCUT_OPTION_UNAVAILABLE) {
      fail("Home/Reader was exposed on Home-key or long-press controls");
    }
    const uint8_t savedPowerAction = SETTINGS.shortPwrBtn;
    const uint8_t savedChordAction = SETTINGS.powerChordAction;
    SETTINGS.shortPwrBtn = CrossPointSettings::HOME_READER;
    SETTINGS.powerChordAction = CrossPointSettings::CHORD_HOME_READER;
    JsonDocument shortcutRoundTrip;
    SETTINGS.toJson(shortcutRoundTrip);
    SETTINGS.shortPwrBtn = CrossPointSettings::IGNORE;
    SETTINGS.powerChordAction = CrossPointSettings::CHORD_DISABLED;
    SETTINGS.fromJson(shortcutRoundTrip.as<JsonVariantConst>());
    const uint8_t expectedPowerAction =
        shouldExposeHomeReader ? CrossPointSettings::HOME_READER : CrossPointSettings::IGNORE;
    const uint8_t expectedChordAction =
        shouldExposeHomeReader ? CrossPointSettings::CHORD_HOME_READER : CrossPointSettings::CHORD_DISABLED;
    if (SETTINGS.shortPwrBtn != expectedPowerAction || SETTINGS.powerChordAction != expectedChordAction) {
      fail("Home/Reader settings round-trip did not match device availability");
    }
    SETTINGS.shortPwrBtn = savedPowerAction;
    SETTINGS.powerChordAction = savedChordAction;
    if (!gpio.hasHomeKey() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_HOME_BUTTON) != chordSetting->enumRawValues.end()) {
      fail("Toggle Home Button is still offered without a Home key");
    }
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_PREVIOUS_PAGE) == chordSetting->enumRawValues.end()) {
      fail("Previous Page was removed by an unrelated power-button action ID");
    }
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_BOOKORBIT_SYNC) == chordSetting->enumRawValues.end()) {
      fail("BookOrbit Sync is missing from the Power + Up chord setting");
    }
    {
      // Up to v1.6.0+bookorbit.2, BookOrbit Sync was saved under the numbers CrossInk v1.6.1
      // gives Library. A file from before v1.6.1 carries them over; one saved by v1.6.1,
      // which has the side-button keys, keeps its Library shortcuts.
      JsonDocument current;
      SETTINGS.toJson(current);
      JsonDocument legacyFork;
      legacyFork.set(current);
      legacyFork.remove("bookOrbitShortcutIds");
      legacyFork.remove("sideButtonUpShort");
      legacyFork["longPwrBtn"] = 33;
      legacyFork["powerChordAction"] = 31;
      legacyFork["longPressMenuAction"] = 24;
      SETTINGS.fromJson(legacyFork.as<JsonVariantConst>());
      if (SETTINGS.longPwrBtn != CrossPointSettings::BOOKORBIT_SYNC ||
          SETTINGS.powerChordAction != CrossPointSettings::CHORD_BOOKORBIT_SYNC ||
          SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_BOOKORBIT_SYNC) {
        fail("BookOrbit Sync shortcuts saved before v1.6.1 were not carried over");
      }
      JsonDocument upstream;
      upstream.set(current);
      upstream.remove("bookOrbitShortcutIds");
      upstream["longPwrBtn"] = 33;
      upstream["powerChordAction"] = 31;
      upstream["longPressMenuAction"] = 24;
      SETTINGS.fromJson(upstream.as<JsonVariantConst>());
      if (SETTINGS.longPwrBtn != CrossPointSettings::LIBRARY ||
          SETTINGS.powerChordAction != CrossPointSettings::CHORD_LIBRARY ||
          SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_LIBRARY) {
        fail("Library shortcuts saved by v1.6.1 were mistaken for BookOrbit Sync");
      }
      SETTINGS.fromJson(current.as<JsonVariantConst>());
    }
    if (!gpio.hasTouch() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_TOUCHSCREEN) != chordSetting->enumRawValues.end()) {
      fail("Toggle Touchscreen is still offered without touch hardware");
    }
    if (!Frontlight.present() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_FRONTLIGHT) != chordSetting->enumRawValues.end()) {
      fail("Toggle Frontlight is still offered without a frontlight");
    }

    const auto hasLibrary = [](const SettingInfo& setting, const ShortcutOptionCatalog catalog) {
      const auto raw = shortcutRawValue(catalog, CrossPointSettings::LIBRARY);
      const auto choice = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), raw);
      return choice != setting.enumRawValues.end() &&
             setting.enumValues[static_cast<size_t>(choice - setting.enumRawValues.begin())] == StrId::STR_LIBRARY;
    };
    const auto verifyLibraryChoice = [&](const char* key, const ShortcutOptionCatalog catalog) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == allSettings.end() || !hasLibrary(*setting, catalog)) {
        fail("Library shortcut is missing or mislabeled in %s", key);
      }
    };
    verifyLibraryChoice("shortPwrBtn", ShortcutOptionCatalog::PowerButton);
    verifyLibraryChoice("longPwrBtn", ShortcutOptionCatalog::PowerButton);
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      verifyLibraryChoice(key, ShortcutOptionCatalog::SideButton);
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      for (const auto [action, suffix] :
           {std::pair{CrossPointSettings::SIDE_ROTATE_COUNTERCLOCKWISE, StrId::STR_ROTATE_CCW},
            std::pair{CrossPointSettings::SIDE_ROTATE_CLOCKWISE, StrId::STR_ROTATE_CW},
            std::pair{CrossPointSettings::SIDE_ROTATE_FLIP, StrId::STR_ROTATE_FLIP}}) {
        const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), action);
        const std::string expected = I18N.get(suffix);
        if (choice == setting->enumRawValues.end() ||
            sideButtonOptionLabel(*setting, static_cast<uint8_t>(choice - setting->enumRawValues.begin())) !=
                expected) {
          fail("Orientation shortcut is missing or mislabeled in %s", key);
        }
      }
    }
    for (uint8_t orientation = 0; orientation < CrossPointSettings::ORIENTATION_COUNT; ++orientation) {
      if (ReaderUtils::flippedOrientation(ReaderUtils::flippedOrientation(orientation)) != orientation ||
          ReaderUtils::flippedOrientation(orientation) !=
              ReaderUtils::rotatedOrientation(ReaderUtils::rotatedOrientation(orientation, true), true)) {
        fail("Flip must turn the screen 180 degrees from every orientation");
      }
    }
    verifyLibraryChoice("powerChordAction", ShortcutOptionCatalog::ButtonChord);
    verifyLibraryChoice("longPressMenuAction", ShortcutOptionCatalog::LongPress);
    verifyLibraryChoice("longPressBackAction", ShortcutOptionCatalog::LongPress);
    const auto verifySleepChoice = [&](const char* key, const ShortcutOptionCatalog catalog) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      const auto raw = shortcutRawValue(catalog, CrossPointSettings::SLEEP);
      if (setting == allSettings.end() || raw == SHORTCUT_OPTION_UNAVAILABLE) {
        fail("Sleep shortcut is missing from %s", key);
      }
      const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), raw);
      const std::string expected = catalog == ShortcutOptionCatalog::PowerButton
                                       ? std::string(tr(STR_SLEEP)) + "/" + tr(STR_WAKE)
                                       : tr(STR_SLEEP);
      if (choice == setting->enumRawValues.end() ||
          sideButtonOptionLabel(*setting, static_cast<uint8_t>(choice - setting->enumRawValues.begin())) != expected) {
        fail("Sleep shortcut has the wrong label in %s", key);
      }
    };
    verifySleepChoice("shortPwrBtn", ShortcutOptionCatalog::PowerButton);
    verifySleepChoice("longPwrBtn", ShortcutOptionCatalog::PowerButton);
    for (const char* key : {"shortPwrBtn", "longPwrBtn"}) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == allSettings.end()) fail("Power shortcut setting is missing: %s", key);
      for (const auto [action, label] : {std::pair{CrossPointSettings::SLEEP_ONLY, StrId::STR_SLEEP},
                                         std::pair{CrossPointSettings::WAKE_ONLY, StrId::STR_WAKE}}) {
        const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), action);
        if (choice == setting->enumRawValues.end() ||
            sideButtonOptionLabel(*setting, static_cast<uint8_t>(choice - setting->enumRawValues.begin())) !=
                I18N.get(label)) {
          fail("Power-only shortcut is missing or mislabeled in %s", key);
        }
      }
    }
    const uint8_t savedShortPowerAction = SETTINGS.shortPwrBtn;
    for (const auto [action, wakes] :
         {std::pair{CrossPointSettings::IGNORE, false}, std::pair{CrossPointSettings::SLEEP_ONLY, false},
          std::pair{CrossPointSettings::WAKE_ONLY, true}, std::pair{CrossPointSettings::SLEEP, true}}) {
      SETTINGS.shortPwrBtn = action;
      if (SETTINGS.shortPowerPressWakes() != wakes) fail("Short Power wake policy does not match its shortcut");
    }
    SETTINGS.shortPwrBtn = savedShortPowerAction;
    verifySleepChoice("powerChordAction", ShortcutOptionCatalog::ButtonChord);
    verifySleepChoice("longPressMenuAction", ShortcutOptionCatalog::LongPress);
    verifySleepChoice("longPressBackAction", ShortcutOptionCatalog::LongPress);
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      const auto side = std::find_if(sideButtonSettings.begin(), sideButtonSettings.end(),
                                     [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (side != sideButtonSettings.end()) {
        const auto sleep = std::find(side->enumRawValues.begin(), side->enumRawValues.end(), CrossPointSettings::SLEEP);
        if (sleep == side->enumRawValues.end() ||
            sideButtonOptionLabel(*side, static_cast<uint8_t>(sleep - side->enumRawValues.begin())) != tr(STR_SLEEP)) {
          fail("Sleep shortcut has the wrong label in %s", key);
        }
      }
    }
    if (gpio.hasHomeKey()) {
      verifyLibraryChoice("homeButtonTapAction", ShortcutOptionCatalog::HomeButton);
      verifyLibraryChoice("homeButtonDoubleTapAction", ShortcutOptionCatalog::HomeButton);
      verifyLibraryChoice("homeButtonLongPressAction", ShortcutOptionCatalog::HomeButton);
      verifySleepChoice("homeButtonTapAction", ShortcutOptionCatalog::HomeButton);
      verifySleepChoice("homeButtonDoubleTapAction", ShortcutOptionCatalog::HomeButton);
      verifySleepChoice("homeButtonLongPressAction", ShortcutOptionCatalog::HomeButton);
    }
    if (hasSideButtonChord) {
      const auto side =
          std::find_if(sideButtonSettings.begin(), sideButtonSettings.end(),
                       [](const SettingInfo& setting) { return settingKeyIs(setting, "sideButtonChordAction"); });
      if (side == sideButtonSettings.end() || !hasLibrary(*side, ShortcutOptionCatalog::ButtonChord)) {
        fail("Library shortcut is missing from the filtered Up + Down choices");
      }
      const auto sleep =
          std::find(side->enumRawValues.begin(), side->enumRawValues.end(), CrossPointSettings::CHORD_SLEEP);
      if (sleep == side->enumRawValues.end() ||
          sideButtonOptionLabel(*side, static_cast<uint8_t>(sleep - side->enumRawValues.begin())) != tr(STR_SLEEP)) {
        fail("Sleep shortcut is missing or mislabeled in the filtered Up + Down choices");
      }
    }
    if (!QuickActions::isQuickActionSlotActionAvailable(CrossPointSettings::LIBRARY) ||
        QuickActions::actionLabel(CrossPointSettings::LIBRARY) != StrId::STR_LIBRARY) {
      fail("Library is missing from Quick Actions choices");
    }

    const uint8_t savedTrackReadingStats = SETTINGS.trackReadingStats;
    SETTINGS.trackReadingStats = 0;
    const auto statsDisabledSettings = getSettingsList();
    SETTINGS.trackReadingStats = savedTrackReadingStats;
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      const auto setting = std::find_if(statsDisabledSettings.begin(), statsDisabledSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == statsDisabledSettings.end() ||
          std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), CrossPointSettings::READING_STATS) !=
              setting->enumRawValues.end()) {
        fail("Reading Stats remains available in %s while tracking is disabled", key);
      }
    }
  }

  static void verifySideButtonMigrationAndInput() {
    JsonDocument original;
    SETTINGS.toJson(original);
    JsonDocument legacy;
    SETTINGS.toJson(legacy);
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      legacy[key] = nullptr;
    }
    legacy["sideButtonLayout"] = CrossPointSettings::NEXT_PREV;
    legacy["sideButtonLongPress"] = CrossPointSettings::SIDE_LONG_CHAPTER_SKIP;
    SETTINGS.fromJson(legacy.as<JsonVariantConst>());
    if (SETTINGS.sideButtonUpShort != CrossPointSettings::PAGE_TURN ||
        SETTINGS.sideButtonDownShort != CrossPointSettings::PREVIOUS_PAGE ||
        SETTINGS.sideButtonUpLong != CrossPointSettings::SIDE_NEXT_CHAPTER ||
        SETTINGS.sideButtonDownLong != CrossPointSettings::SIDE_PREVIOUS_CHAPTER)
      fail("Swapped side-button layout migration failed");

    legacy["sideButtonLayout"] = CrossPointSettings::SIDE_BUTTONS_DISABLED;
    legacy["sideButtonLongPress"] = CrossPointSettings::SIDE_LONG_FONT_SIZE;
    SETTINGS.fromJson(legacy.as<JsonVariantConst>());
    if (SETTINGS.sideButtonUpShort != CrossPointSettings::IGNORE ||
        SETTINGS.sideButtonDownShort != CrossPointSettings::IGNORE ||
        SETTINGS.sideButtonUpLong != CrossPointSettings::SIDE_INCREASE_FONT ||
        SETTINGS.sideButtonDownLong != CrossPointSettings::SIDE_DECREASE_FONT)
      fail("Disabled layout with font hold migration failed");

    legacy["sideButtonUpShort"] = CrossPointSettings::LIBRARY;
    SETTINGS.fromJson(legacy.as<JsonVariantConst>());
    if (SETTINGS.sideButtonUpShort != CrossPointSettings::LIBRARY ||
        SETTINGS.sideButtonDownShort != CrossPointSettings::IGNORE)
      fail("Partially migrated side-button choices were overwritten");

    SETTINGS.fromJson(original.as<JsonVariantConst>());
    const auto sideSettings = buildControlsSideButtonSettingsList(getSettingsList());
    const size_t expected = 7 + (deviceSupportsSideButtonChord(gpio) ? 1u : 0u);
    if (sideSettings.size() != expected) fail("Side-button menu row count mismatch");
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      if (std::find_if(sideSettings.begin(), sideSettings.end(),
                       [key](const SettingInfo& setting) { return settingKeyIs(setting, key); }) == sideSettings.end())
        fail("Side-button menu is missing an individual shortcut");
    }

    SideButtonShortcuts shortcuts;
    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Up);
    const auto heldUp = shortcuts.update(mappedInputManager, 1000);
    if (!heldUp.consumed) fail("Side-button press was not captured");
    mappedInputManager.injectRelease(MappedInputManager::Button::Right);
    if (SideButtonShortcuts::shouldConsume(heldUp, mappedInputManager))
      fail("Side-button hold swallowed a chord page turn");
    mappedInputManager.clearInjectedReleases();
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Up);
    const auto shortResult = shortcuts.update(mappedInputManager, 1200);
    if (!shortResult.triggered || shortResult.longPress || shortResult.action != SETTINGS.sideButtonUpShort)
      fail("Side-button short action did not dispatch once");
    mappedInputManager.simulatorClearInputFrame();
    if (shortcuts.update(mappedInputManager, 1300).triggered) fail("Side-button release dispatched twice");

    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Down);
    shortcuts.update(mappedInputManager, 2000);
    mappedInputManager.simulatorClearInputFrame();
    const auto longResult = shortcuts.update(mappedInputManager, 2800);
    if (!longResult.triggered || !longResult.longPress || longResult.action != SETTINGS.sideButtonDownLong)
      fail("Side-button long action did not dispatch");
    mappedInputManager.suppressNextSideRelease(MappedInputManager::Button::Down);
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Down) ||
        shortcuts.update(mappedInputManager, 2900).triggered)
      fail("Side-button long release leaked into the next activity");
    mappedInputManager.simulatorClearInputFrame();
    if (shortcuts.update(mappedInputManager, 3000).triggered) fail("Side-button long action dispatched twice");

    ButtonShortcutController lock;
    lock.toggleQuickLock(4000, QuickLockTrigger::SideUpShort);
    if (lock.tryUnlockSide(4100, true, true, false, true, false, false, false, true, ReaderUtils::SKIP_HOLD_MS) ||
        !lock.isQuickLocked())
      fail("Side-button Quick Lock unlocked on press instead of release");
    if (!lock.tryUnlockSide(4200, false, false, true, true, false, false, false, true, ReaderUtils::SKIP_HOLD_MS) ||
        lock.isQuickLocked())
      fail("Side-button short Quick Lock did not unlock on a fresh release");
    lock.toggleQuickLock(5000, QuickLockTrigger::SideDownLong);
    if (lock.tryUnlockSide(5100, false, false, false, true, true, false, false, true, ReaderUtils::SKIP_HOLD_MS))
      fail("An inherited side-button hold unlocked Quick Lock");
    lock.tryUnlockSide(5200, false, false, false, true, false, false, true, true, ReaderUtils::SKIP_HOLD_MS);
    lock.tryUnlockSide(5300, false, false, false, true, true, true, false, true, ReaderUtils::SKIP_HOLD_MS);
    if (!lock.tryUnlockSide(6000, false, false, false, true, true, false, false, true, ReaderUtils::SKIP_HOLD_MS) ||
        lock.isQuickLocked())
      fail("Side-button long Quick Lock did not unlock on a fresh hold");
    LOG_INF("SMOKE", "Side-button migration and press/release checks passed");
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

  void verifyLoadingPopupBackdrop() {
    RenderLock lock;
    const auto originalOrientation = renderer.getOrientation();
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      renderer.clearScreen();
      for (int y = 0; y < height; y += 7) renderer.drawLine(0, y, width - 1, y);
      const size_t bytes = renderer.getRegionByteSize(0, 0, width, height);
      std::vector<uint8_t> before(bytes), after(bytes);
      if (!renderer.copyRegionToBuffer(0, 0, width, height, before.data(), bytes))
        fail("Could not snapshot loading popup backdrop");
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), true);
      if (!renderer.copyRegionToBuffer(0, 0, width, height, after.data(), bytes) || before != after)
        fail("Loading popup changed the underlying framebuffer");
    }
    renderer.setOrientation(originalOrientation);
    renderer.clearScreen();
    LOG_INF("SMOKE", "Loading popup preserves backdrop in all orientations");
  }

  static void verifyWakePowerReaderShortcut() {
    if (!activityManager.isCurrentActivityNamed("EpubReader")) return;
    auto* reader = activityManager.simulatorCurrentActivity();
    const uint8_t savedShort = SETTINGS.shortPwrBtn;
    const uint8_t savedLong = SETTINGS.longPwrBtn;
    const uint8_t savedSlot = SETTINGS.quickActionSlots[0];
    SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK;
    SETTINGS.longPwrBtn = CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS;
    SETTINGS.quickActionSlots[0] = CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH;

    // Apply the same mapped-input guards as a Power-button wake, then exercise
    // the reader's independent long-press route across the hold and release.
    mappedInputManager.suppressNextPowerRelease();
    mappedInputManager.suppressNextPowerConfirmRelease();
    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Power);
    mappedInputManager.update();
    delay(SETTINGS.getPowerButtonLongPressDuration() + 10);
    if (!mappedInputManager.isPhysicalPressed(MappedInputManager::Button::Power)) fail("Wake test lost Power hold");
    reader->loop();
    if (reader->blocksGlobalInput()) fail("Wake Power hold opened Quick Actions");

    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Power);
    mappedInputManager.update();
    reader->loop();
    if (reader->blocksGlobalInput()) fail("Wake Power release opened Quick Actions");
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.update();

    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Power);
    mappedInputManager.update();
    delay(SETTINGS.getPowerButtonLongPressDuration() + 10);
    reader->loop();
    if (!reader->blocksGlobalInput()) fail("Fresh long Power press did not open Quick Actions");
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Power);
    mappedInputManager.update();
    reader->loop();
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.update();
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
    reader->loop();
    mappedInputManager.simulatorClearInputFrame();
    if (reader->blocksGlobalInput()) fail("Wake test could not close Quick Actions");
    SETTINGS.shortPwrBtn = savedShort;
    SETTINGS.longPwrBtn = savedLong;
    SETTINGS.quickActionSlots[0] = savedSlot;
    LOG_INF("SMOKE", "Wake Power hold/release ignored; fresh long press opens Quick Actions");
  }

  // This fork's Cloudflare Access token: what the BookOrbit store keeps, forgets and sends.
  static void verifyBookOrbitCloudflareToken() {
    auto& store = BOOKORBIT_STORE;
    JsonDocument saved;
    store.toJson(saved);

    store.setServerUrl("books.example.com");
    store.setCfClientId("  0123abcd.access\n");
    store.setCfClientSecret(" s3cret\r\n");
    if (store.getCfClientId() != "0123abcd.access" || store.getCfClientSecret() != "s3cret") {
      fail("Cloudflare Access token was not trimmed");
    }
    if (!store.sendsCfAccessToken()) fail("Cloudflare Access token withheld from an https server");
    store.setServerUrl("http://192.168.1.10:3010");
    if (store.sendsCfAccessToken()) fail("Cloudflare Access token offered to a plain http server");

    JsonDocument written;
    store.toJson(written);
    const std::string obfuscated = written["cfClientSecret_obf"] | "";
    if (obfuscated.empty() || obfuscated == "s3cret" || !written["cfClientSecret"].isNull()) {
      fail("Cloudflare Access secret was not written obfuscated");
    }
    store.setCfClientSecret("");
    if (!store.fromJson(written.as<JsonVariantConst>()) || store.getCfClientId() != "0123abcd.access" ||
        store.getCfClientSecret() != "s3cret") {
      fail("Cloudflare Access token did not survive a save and reload");
    }

    // A secret typed into the file by hand is valid base64 without being this device's
    // encoding: it must be dropped, not decoded into random bytes and sent.
    written["cfClientSecret_obf"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    if (!store.fromJson(written.as<JsonVariantConst>()) || !store.getCfClientSecret().empty()) {
      fail("Cloudflare Access secret was accepted without this device's encoding");
    }

    store.setCfClientSecret("s3cret");
    store.setCfClientId(" ");
    if (!store.getCfClientSecret().empty() || store.sendsCfAccessToken()) {
      fail("Emptying the Cloudflare Access Client ID kept its secret");
    }

    store.fromJson(saved.as<JsonVariantConst>());
    LOG_INF("SMOKE", "BookOrbit Cloudflare Access token contract holds");
  }

  static void verifyCachedHomeProgressMigration() {
    const RecentBook book{"/books/legacy-home-smoke.epub", "Legacy Home smoke", {}, {}};
    const std::string legacy = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book.path));
    const std::string current = Epub::cachePathForFilePath(book.path, "/.crosspoint");
    if (legacy == current || Storage.exists(book.path.c_str())) fail("Invalid legacy Home fixture");
    if (!Storage.mkdir(legacy.c_str())) fail("Cannot create legacy Home cache");
    RecentBookProgress::saveCachedEpubPercent(legacy, 42.5f);
    // There is deliberately no EPUB or book.bin. This must migrate and read the
    // tiny saved percentage without attempting to open, parse or index a book.
    if (RecentBookProgress::loadCachedEpubPercent(book) != 42.5f || Storage.exists(legacy.c_str()) ||
        !Storage.exists(current.c_str()) || BookMetadataCache::exists(current))
      fail("Home did not recover legacy cached progress without opening the EPUB");
    const std::string percentFile = current + "/progress_percent.bin";
    const std::string hostPercentFile = "fs_" + percentFile;
    // Force an old timestamp so this check does not depend on filesystem clock resolution.
    const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    std::filesystem::last_write_time(hostPercentFile, oldTime);
    const auto savedTime = std::filesystem::last_write_time(hostPercentFile);
    RecentBookProgress::saveCachedEpubPercent(current, 42.5001f);
    if (std::filesystem::last_write_time(hostPercentFile) != savedTime)
      fail("Saving the same rounded Home percentage rewrote the cache");
    RecentBookProgress::saveCachedEpubPercent(current, 43.5f);
    if (RecentBookProgress::loadCachedEpubPercent(book) != 43.5f) fail("Changed Home percentage was not saved");
    std::filesystem::resize_file(hostPercentFile, 2);
    RecentBookProgress::saveCachedEpubPercent(current, 43.5f);
    if (RecentBookProgress::loadCachedEpubPercent(book) != 43.5f || std::filesystem::file_size(hostPercentFile) != 7)
      fail("Saving Home percentage did not repair a truncated cache");
    if (!Storage.remove(percentFile.c_str())) fail("Cannot remove Home percent fixture");
    RecentBookProgress::saveCachedEpubPercent(current, 42.5f);
    LOG_INF("SMOKE", "Home percentage skips unchanged writes and repairs missing/truncated caches");
    if (!Storage.mkdir(legacy.c_str())) fail("Cannot recreate stale legacy Home cache");
    RecentBookProgress::saveCachedEpubPercent(legacy, 90.0f);
    if (RecentBookProgress::loadCachedEpubPercent(book) != 42.5f || !Storage.exists(legacy.c_str()))
      fail("Stale legacy progress replaced the current Home cache");
    if (!Storage.removeDir(legacy.c_str()) || !Storage.removeDir(current.c_str())) fail("Cannot remove Home fixtures");
    LOG_INF("SMOKE", "Legacy Home progress migration without EPUB loading passed");
  }

  static uint64_t hashBytes(const uint8_t* bytes, size_t size, uint64_t hash = 14695981039346656037ull) {
    for (size_t i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
    return hash;
  }

  static uint64_t hashCarouselCache() {
    FsFile file;
    if (!Storage.openFileForRead("SMOKE", "/.crosspoint/home_carousel_cache_0.bin", file)) {
      fail("Carousel did not publish an artwork cache");
    }
    if (file.size() < renderer.getBufferSize() || file.size() >= 2 * renderer.getBufferSize()) {
      file.close();
      fail("Carousel snapshot must contain only one viewed artwork frame");
    }
    uint64_t hash = 14695981039346656037ull;
    uint8_t bytes[128];
    int count;
    while ((count = file.read(bytes, sizeof(bytes))) > 0) hash = hashBytes(bytes, count, hash);
    file.close();
    return hash;
  }

  void queueCarouselSwipe(bool next) {
    ++carouselCachePass;
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::CarouselHome;
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouchHardware()) {
      const int startX = renderer.getScreenWidth() * (next ? 3 : 1) / 4;
      const int endX = renderer.getScreenWidth() * (next ? 1 : 3) / 4;
      const int y = renderer.getScreenHeight() / 2;
      inputScript.push_back(touchDown(startX, y));
      inputScript.push_back(touchMove(endX, y));
      inputScript.push_back(touchRelease(endX, y));
    } else
#endif
    {
      addTap(next ? MappedInputManager::Button::Right : MappedInputManager::Button::Left);
    }
    inputScript.push_back(render("Carousel viewed position", 8));
    step = SmokeStep::ReaderInput;
  }

  void verifyCarouselCacheReturn() {
    const auto writtenAt = std::filesystem::last_write_time("fs_/.crosspoint/home_carousel_cache_0.bin");
    const uint64_t cacheHash = hashCarouselCache();
    uint64_t screenHash;
    {
      RenderLock lock;
      screenHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
    }
    if (carouselCachePass == 0) {
      if (Storage.exists("/.crosspoint/home_carousel_cache_1.bin") ||
          Storage.exists("/.crosspoint/home_carousel_cache_2.bin")) {
        fail("Carousel eagerly prepared unviewed positions");
      }
      carouselCacheWrittenAt = writtenAt;
      carouselCacheHash = cacheHash;
      const RecentBook& book = RECENT_BOOKS.getBooks().front();
      const std::string cachePath = Epub::resolveCachePathForFilePath(book.path, "/.crosspoint");
      const float oldProgress = RecentBookProgress::loadCachedEpubPercent(book);
      RecentBookProgress::saveCachedEpubPercent(cachePath, oldProgress < 50.0f ? 75.0f : 25.0f);
      BookReadingStats stats = BookReadingStats::load(cachePath);
      stats.sessionCount = 1;
      stats.totalReadingSeconds += 7200;
      if (!stats.save(cachePath)) fail("Cannot save carousel stats fixture");
    } else if (carouselCachePass <= 2) {
      if (writtenAt != carouselCacheWrittenAt || cacheHash != carouselCacheHash) {
        fail("Reading progress/stats or tracking settings rebuilt the carousel artwork");
      }
      if (screenHash == carouselScreenHash) fail("Carousel restored stale progress/stats or menu pixels");
      if (carouselCachePass == 1) {
        SETTINGS.trackReadingStats = 0;
      } else {
        SETTINGS.screenInverted = !SETTINGS.screenInverted;
      }
    } else if (carouselCachePass == 3) {
      if (cacheHash == carouselCacheHash) fail("Dark Mode did not invalidate carousel artwork");
      carouselCacheHash = cacheHash;
      const RecentBook book = RECENT_BOOKS.getBooks().front();
      if (!RECENT_BOOKS.updateBook(book.path, "Changed carousel title", book.author, book.coverBmpPath,
                                   book.coverState))
        fail("Cannot update carousel title fixture");
    } else if (carouselCachePass == 4) {
      if (cacheHash == carouselCacheHash) fail("Title changes did not invalidate carousel artwork");
      carouselCacheHash = cacheHash;
      carouselCacheWrittenAt = writtenAt;
      queueCarouselSwipe(true);
      return;
    } else {
      if (cacheHash != carouselCacheHash || writtenAt != carouselCacheWrittenAt) {
        fail("Navigating the carousel rewrote an already cached position");
      }
      const bool secondExists = Storage.exists("/.crosspoint/home_carousel_cache_1.bin");
      if (!secondExists || Storage.exists("/.crosspoint/home_carousel_cache_2.bin")) {
        fail("Carousel navigation did not cache only the viewed position");
      }
      const auto secondWrittenAt = std::filesystem::last_write_time("fs_/.crosspoint/home_carousel_cache_1.bin");
      const int selected = carouselCachePass == 5 ? 1 : 0;
      if (activityManager.getCurrentBookPath() != RECENT_BOOKS.getBooks()[selected].path) {
        fail("Carousel swipe did not select the expected book");
      }
      if (carouselCachePass == 5) {
        carouselSecondWrittenAt = secondWrittenAt;
        queueCarouselSwipe(false);
        return;
      }
      if (secondWrittenAt != carouselSecondWrittenAt) fail("Carousel rewrote the previous position while leaving it");
      LOG_INF("SMOKE", "Carousel lazy cache, live progress/stats/menu, Dark Mode and title invalidation passed");
      step = SmokeStep::Done;
      return;
    }
    carouselScreenHash = screenHash;
    ++carouselCachePass;
    activityManager.goHome();
    queueStep("Carousel return cache", SmokeStep::CarouselHome, 8);
  }

#if CROSSINK_SCALABLE_FONTS
  void verifyBlockFontSizes(const int readerFontId) {
    RenderLock lock;
    const int baseFont = renderer.getFontIdForSize(readerFontId, 12);
    if (renderer.getFontPointSize(baseFont) != 12) fail("Missing scalable body size");
    for (const uint8_t points : {8, 12, 24, 32, 44}) {
      if (renderer.getFontPointSize(renderer.getFontIdForSize(baseFont, points)) != points)
        fail("Missing scalable content size %u", unsigned(points));
    }
    const std::string path = "/block-font-smoke.xhtml";
    if (!Storage.writeFile(path.c_str(),
                           "<html><body><h1>Heading Heading Heading Heading Heading</h1>"
                           "<p style=\"font-size:150%\">Large Large Large</p><p>Body Body Body</p></body></html>"))
      fail("Cannot write block font fixture");
    Epub book("/block-font-smoke.epub", "/.crosspoint");
    CssParser css("/.crosspoint/block-font-smoke");
    unsigned checked = 0;
    ChapterHtmlSlimParser parser(
        book, path, renderer, baseFont, 1.0f, false, false, 1, 320, 600, false, false, false, 0,
        [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
          FsFile file;
          if (!Storage.openFileForWrite("SMOKE", "/block-font-page", file) || !page->serialize(file))
            fail("Cannot save sized page");
          file.close();
          if (!Storage.openFileForRead("SMOKE", "/block-font-page", file)) fail("Cannot reload sized page");
          auto restored = Page::deserialize(file);
          file.close();
          if (!restored) fail("Sized page cache did not round trip");
          int bottom = 0;
          for (const auto& element : restored->elements) {
            if (element->getTag() != TAG_PageLine) continue;
            const auto& line = static_cast<const PageLine&>(*element);
            const auto& block = *line.getBlock();
            if (!block.wordCount()) continue;
            const char* word = block.wordText(0);
            const int expected = std::strcmp(word, "Heading") == 0 ? 24 : std::strcmp(word, "Large") == 0 ? 18 : 12;
            const int actualFont = block.resolvedFontId(renderer, baseFont);
            if (block.getBlockStyle().fontSize != expected || renderer.getFontPointSize(actualFont) != expected ||
                block.getBlockStyle().lineHeight != renderer.getLineHeight(actualFont))
              fail("Sized page metrics mismatch: %s", word);
            if (line.yPos < bottom || line.yPos + block.getBlockStyle().lineHeight > 600)
              fail("Sized page lines overlap or overflow");
            bottom = line.yPos + block.getBlockStyle().lineHeight;
            ++checked;
          }
          restored->renderText(renderer, baseFont, 0, 0);
        },
        true, "", "", 0, {}, nullptr, &css);
    if (!parser.parseAndBuildPages() || checked < 3) fail("Block font parser smoke failed");
    WordSelectNavigator navigator;
    WordSelectNavigator::WordInfo word;
    word.textLen = word.lookupLen = 7;
    word.screenX = 20;
    word.screenY = 20;
    word.width = 100;
    word.setLineHeight(60);
    word.fontId = renderer.getFontIdForSize(baseFont, 24);
    navigator.load({word}, {{20, 0, 1}}, "Heading", false);
    bool hit = false;
    navigator.selectWordAtPoint(50, 75, 20, &hit);
    if (!hit) fail("Heading word selection used body height");
    Storage.remove(path.c_str());
    Storage.remove("/block-font-page");
    LOG_INF("SMOKE", "Block font sizes: layout, cache, rendering and selection passed");
  }
#endif

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
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BARS")) {
          verifyStatusBarSettings();
          SETTINGS.clockDateHasBeenSynced = true;
          SETTINGS.dateFormat = CrossPointSettings::DATE_FORMAT_YEAR_MONTH_DAY_NUMERIC;
          SETTINGS.dateSeparator = CrossPointSettings::DATE_SEPARATOR_HYPHEN;
          SETTINGS.displayStatusBar.slots = {ReaderStatusBarItem::Clock, ReaderStatusBarItem::Date,
                                             ReaderStatusBarItem::Battery};
          activityManager.replaceActivity(
              std::make_unique<StatusBarSettingsActivity>(renderer, mappedInputManager, false, false, true));
          queueStep("Display status bar editor", SmokeStep::StatusBarEditor, 4);
          break;
        }

        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_HOME_THEMES")) {
          if (!mappedInputManager.hasHomeKey() || !mappedInputManager.hasTouchHardware())
            fail("Home theme regression requires the X4 Pro simulator");
          for (const char* path : {"/books/theme-first.txt", "/books/theme-second.txt"}) {
            if (!Storage.writeFile(path, "Home theme fixture")) fail("Cannot create Home theme fixture");
            RECENT_BOOKS.addOrUpdateBook(path, path, {}, {}, RecentBook::CoverState::Missing);
          }
          SETTINGS.uiTheme = CrossPointSettings::LYRA_CAROUSEL;
          SETTINGS.uiScale = CrossPointSettings::UI_SCALE_SMALL;
          UITheme::getInstance().reload();
          homeThemeBookPath = "/books/theme-first.txt";
          activityManager.replaceActivity(std::make_unique<HomeActivity>(
              renderer, mappedInputManager, HomeMenuItem::NONE, HalDisplay::FAST_REFRESH, homeThemeBookPath));
          queueStep("Initial carousel Home", SmokeStep::ThemeHome, 8);
          break;
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_LAYOUT")) {
          if (!mappedInputManager.hasHomeKey() || !mappedInputManager.hasTouchHardware())
            fail("Frontlight layout regression requires the X4 Pro simulator");
          activityManager.replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInputManager));
          queueStep("Frontlight layout Home", SmokeStep::FrontlightLayout, 4);
          break;
        }
        verifyLoadingPopupBackdrop();
        verifyCachedHomeProgressMigration();
        verifyBookOrbitCloudflareToken();
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
        verifySideButtonMigrationAndInput();
        verifyReaderControlsSettings();
        verifyStatusBarSettings();
        verifyMixedPageGestures();
#if CROSSINK_SCALABLE_FONTS
        verifyBlockFontSizes(sdFontSystem.ensureBuiltInReaderFont(renderer));
        if (const char* family = std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_FAMILY")) {
          // Exercise the production registry, adapter, size cache, and dictionary
          // handoff before the normal reader navigation smoke sequence.
          sdFontSystem.ensureRegistry();
          sdFontSystem.releaseRegistry();
          sdFontSystem.ensureRegistry();
          for (const auto& summary : sdFontSystem.registry().getFamilies()) {
            if (!summary.files.empty()) fail("Font names eagerly loaded detail paths");
          }
          const auto* info = sdFontSystem.registry().findFamily(family);
          if (!info || !info->isScalable()) fail("TTF family not discovered: %s", family);
          const auto picker = buildFontFamilySetting(&sdFontSystem.registry());
          const std::string expectedLabel = std::string(family) + " (8-22pt)";
          const auto item = std::find(picker.enumStringValues.begin(), picker.enumStringValues.end(), expectedLabel);
          if (item == picker.enumStringValues.end()) fail("TTF family missing from picker: %s", family);
          picker.valueSetter(static_cast<uint8_t>(item - picker.enumStringValues.begin()));
          if (std::strcmp(SETTINGS.sdFontFamilyName, family) != 0) fail("TTF picker selected the wrong family");
          SETTINGS.readerFontPointSize = 12;
          sdFontSystem.ensureLoaded(renderer);
          const int original = SETTINGS.getReaderFontId();
          verifyBlockFontSizes(original);
          const TtfRenderProfile initialProfile = TTF_RENDER_PROFILES.profileFor(family);
          TtfRenderProfile nativeProfile = initialProfile;
          nativeProfile.hinting = 1;  // TtfRenderProfile: Native
          nativeProfile.interpreter = 0;
          if (!TTF_RENDER_PROFILES.setProfile(family, nativeProfile)) fail("TTF native profile was not persisted");
          if (!sdFontSystem.reloadActiveScalableFamily(renderer, family)) fail("TTF native profile reload failed");
          const int nativeId = SETTINGS.getReaderFontId();
          if (nativeId == original || nativeId == SETTINGS.getBuiltInReaderFontId())
            fail("TTF native profile did not replace the resident font identity");
          if (!TTF_RENDER_PROFILES.setProfile(family, initialProfile)) fail("TTF initial profile was not restored");
          if (!sdFontSystem.reloadActiveScalableFamily(renderer, family)) fail("TTF initial profile reload failed");
          if (SETTINGS.getReaderFontId() != original) fail("TTF profile round trip changed font identity");
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Resident TTF family incorrectly requests loading feedback");
          }
          sdFontSystem.releaseLoadedFont(renderer);
          if (!ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Released TTF family suppressed loading feedback");
          }
          sdFontSystem.ensureLoaded(renderer);
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Reloaded TTF family incorrectly requests loading feedback");
          }
          sdFontSystem.releaseRegistry();
          if (!ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Released TTF catalog suppressed loading feedback");
          }
          sdFontSystem.ensureRegistry();
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Reloaded TTF catalog incorrectly requests loading feedback");
          }
          sdFontSystem.markRegistryDirty();
          if (!ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Dirty TTF family suppressed loading feedback");
          }
          sdFontSystem.ensureLoaded(renderer);
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Reloaded dirty TTF family incorrectly requests loading feedback");
          }
          SETTINGS.readerFontPointSize = 22;
          sdFontSystem.releaseRegistry();
          if (ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp) ||
              SETTINGS.readerFontPointSize != 22)
            fail("Cold resize at maximum size did not remain clamped");
          SETTINGS.readerFontPointSize = 12;
          sdFontSystem.releaseRegistry();
          if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp) ||
              SETTINGS.readerFontPointSize != 13)
            fail("Resize did not lazily reload indexed SD sizes");
          sdFontSystem.ensureLoaded(renderer);
          if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, false, FontSizeStepMode::Clamp) ||
              SETTINGS.readerFontPointSize != 12)
            fail("Reverse resize did not use indexed sizes");
          sdFontSystem.ensureLoaded(renderer);
          if (original == SETTINGS.getBuiltInReaderFontId()) fail("TTF activation fell back");
          for (uint8_t points : {uint8_t(14), uint8_t(12)}) {
            SETTINGS.readerFontPointSize = points;
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() == SETTINGS.getBuiltInReaderFontId()) fail("TTF size activation failed");
          }
          if (SETTINGS.getReaderFontId() != original) fail("TTF size identity changed on reuse");
          if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ISOLATED_FONTS")) {
            // A clean resize must use resident metadata, even with the cache temporarily unavailable.
            namespace fs = std::filesystem;
            fs::rename("fs_/.crosspoint/font-catalog.bin", "fs_/.crosspoint/font-catalog.saved");
            if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp))
              fail("Clean resize failed");
            sdFontSystem.ensureLoaded(renderer);
            if (fs::exists("fs_/.crosspoint/font-catalog.bin")) fail("Clean resize reread/rebuilt the index");
            fs::rename("fs_/.crosspoint/font-catalog.saved", "fs_/.crosspoint/font-catalog.bin");
            if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, false, FontSizeStepMode::Clamp))
              fail("Clean reverse resize failed");
            sdFontSystem.ensureLoaded(renderer);
          }
          const auto dictionary = sdFontSystem.activateDictionaryFont(renderer, family, 16);
          if (!dictionary.usingDictionaryFont) fail("TTF dictionary activation failed");
          if (sdFontSystem.restoreReaderFont(renderer) != original) fail("TTF reader restoration changed identity");
          // Cold dictionary activation has no persistent book-layout identity.
          // Restoring that same family must rebuild its normal reader identity.
          sdFontSystem.releaseLoadedFont(renderer);
          const auto temporaryDictionary = sdFontSystem.activateDictionaryFont(renderer, family, 12);
          if (!temporaryDictionary.usingDictionaryFont || temporaryDictionary.fontId == original)
            fail("Cold TTF dictionary did not use a temporary identity");
          const auto reusedDictionary = sdFontSystem.activateDictionaryFont(renderer, family, 12);
          if (reusedDictionary.fontId != temporaryDictionary.fontId)
            fail("Active TTF dictionary family was not reused");
          if (sdFontSystem.restoreReaderFont(renderer) != original)
            fail("Temporary dictionary identity escaped into reader restoration");
          sdFontSystem.releaseLoadedFont(renderer);
          if (!sdFontSystem.activateDictionaryFont(renderer, family, 12).usingDictionaryFont)
            fail("Cold TTF dictionary reload failed");
          sdFontSystem.ensureLoaded(renderer);
          if (SETTINGS.getReaderFontId() != original)
            fail("Reader ensureLoaded reused a temporary dictionary identity");
          sdFontSystem.releaseLoadedFont(renderer);
          sdFontSystem.ensureLoaded(renderer);
          if (SETTINGS.getReaderFontId() != original) fail("TTF reload changed identity");
          if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ISOLATED_FONTS")) {
            namespace fs = std::filesystem;
            // The runner provides disposable copies; never mutate a user's SD tree.
            const auto* current = sdFontSystem.registry().findFamily(family);
            const std::string firstPath = "fs_" + current->files.front().path;
            fs::copy("fs_/fonts", "fs_/font-smoke-backup", fs::copy_options::recursive);
            sdFontSystem.releaseLoadedFont(renderer);
            // Budget failures must preserve selection, release partial faces,
            // and permit a subsequent valid load without stale renderer IDs.
            const auto originalBytes = fs::file_size(firstPath);
            fs::resize_file(firstPath, HalScalableFont::MaxFileBytes + 1);
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() != SETTINGS.getBuiltInReaderFontId() ||
                std::strcmp(SETTINGS.sdFontFamilyName, family) != 0)
              fail("Oversized TTF file did not preserve selection and fall back");
            fs::resize_file(firstPath, originalBytes);
            if (current->files.size() == 4) {
              for (const auto& face : current->files) fs::resize_file("fs_" + face.path, HalScalableFont::MaxFileBytes);
              sdFontSystem.ensureLoaded(renderer);
              if (SETTINGS.getReaderFontId() != SETTINGS.getBuiltInReaderFontId() ||
                  std::strcmp(SETTINGS.sdFontFamilyName, family) != 0)
                fail("Oversized TTF family did not preserve selection and fall back");
              for (const auto& face : current->files) {
                const fs::path target = "fs_" + face.path;
                const auto backup = fs::path("fs_/font-smoke-backup") / fs::relative(target, "fs_/fonts");
                fs::copy_file(backup, target, fs::copy_options::overwrite_existing);
              }
            }
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() != original) fail("TTF budget failure did not recover");
            sdFontSystem.releaseLoadedFont(renderer);
            {
              std::ofstream changed(firstPath, std::ios::binary | std::ios::app);
              changed.put('X');
            }
            sdFontSystem.markRegistryDirty();
            sdFontSystem.refreshIfDirty();  // A picker must not consume the active-font reload signal.
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() == original ||
                SETTINGS.getReaderFontId() == SETTINGS.getBuiltInReaderFontId())
              fail("TTF replacement did not change content identity");
            sdFontSystem.releaseLoadedFont(renderer);
            // Reproduce the source location without interpreting font metadata as a path.
            const fs::path sourceFolder = fs::path(firstPath).parent_path();
            const fs::path duplicateFolder = sourceFolder == fs::path("fs_/fonts")
                                                 ? fs::path("fs_/.fonts")
                                                 : fs::path("fs_/.fonts") / sourceFolder.filename();
            fs::create_directories(duplicateFolder);
            fs::copy_file(firstPath, duplicateFolder / "duplicate.ttf", fs::copy_options::overwrite_existing);
            constexpr const char* separateFamily = "TTF Smoke Separate Family";
            const fs::path separateFolder = fs::path("fs_/fonts") / separateFamily;
            fs::create_directories(separateFolder);
            fs::copy_file(firstPath, separateFolder / "same-metadata.ttf");
            sdFontSystem.markRegistryDirty();
            sdFontSystem.refreshIfDirty();
            if (!sdFontSystem.registry().findFamily(separateFamily)) fail("TTF folder name was ignored");
            FontInstaller installer(sdFontSystem.registry());
            if (installer.deleteFamily(family) != FontInstaller::Error::OK) fail("TTF family deletion failed");
            sdFontSystem.markRegistryDirty();
            sdFontSystem.ensureLoaded(renderer);
            sdFontSystem.refreshIfDirty();
            if (sdFontSystem.registry().findFamily(family) || SETTINGS.sdFontFamilyName[0])
              fail("Deleted TTF family reappeared from duplicate files");
            if (!sdFontSystem.registry().findFamily(separateFamily) ||
                !fs::exists(separateFolder / "same-metadata.ttf"))
              fail("Deleting TTF family removed a different folder with the same metadata");
            fs::remove_all("fs_/fonts");
            fs::rename("fs_/font-smoke-backup", "fs_/fonts");
            sdFontSystem.markRegistryDirty();
            std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", family);
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() != original) fail("Restored TTF content changed identity");
            LOG_INF("SMOKE", "TTF replacement, duplicate deletion and restoration passed");
          }
          LOG_INF("SMOKE", "TTF discovery, size reuse, dictionary and reload passed");
        }
#endif
        applyRequestedTheme();
        homeReaderReaderKind = 0;
        homeReaderCancelledMask = 0;
        homeReaderConfirmationAccepted = false;
        activityManager.replaceActivity(std::make_unique<HomeReaderSmokeActivity>(
            homeReaderSmokeReaderName(homeReaderReaderKind), true, renderer, mappedInputManager, true));
        queueStep("Home/Reader reader owner", SmokeStep::HomeReaderReader);
        break;

      case SmokeStep::HomeReaderReader:
        homeReaderSmokeReader = activityManager.simulatorCurrentActivity();
        if (!homeReaderSmokeReader || !homeReaderSmokeReader->isBookReaderActivity()) {
          fail("Home/Reader smoke reader did not become current");
        }
        homeReaderCancelledMask = 0;
        homeReaderSmokeReader->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("ReaderMenu", true, renderer, mappedInputManager),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) fail("Home/Reader confirmed the first nested menu");
              homeReaderCancelledMask |= 1;
            });
        queueStep("Home/Reader nested menu", SmokeStep::HomeReaderNested);
        break;

      case SmokeStep::HomeReaderNested:
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("NestedSettings", false, renderer, mappedInputManager),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) fail("Home/Reader confirmed nested settings");
              homeReaderCancelledMask |= 2;
            });
        queueStep("Home/Reader destructive confirmation", SmokeStep::HomeReaderConfirmation);
        break;

      case SmokeStep::HomeReaderConfirmation:
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("Confirmation", false, renderer, mappedInputManager),
            [this](const ActivityResult& result) {
              homeReaderConfirmationAccepted = !result.isCancelled;
              homeReaderCancelledMask |= 4;
            });
        queueStep("Home/Reader trigger", SmokeStep::HomeReaderTrigger);
        break;

      case SmokeStep::HomeReaderTrigger:
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER) ||
            !activityManager.handleShortcutAction(CrossPointSettings::HOME_READER)) {
          fail("Repeated Home/Reader press was not consumed");
        }
        queueStep("Home/Reader unwound to reader", SmokeStep::HomeReaderUnwound);
        break;

      case SmokeStep::HomeReaderUnwound:
        if (activityManager.simulatorCurrentActivity() != homeReaderSmokeReader || homeReaderCancelledMask != 7 ||
            homeReaderConfirmationAccepted) {
          fail("Home/Reader did not cancel every nested screen back to the existing reader");
        }
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER)) {
          fail("Home/Reader from the reader was not consumed");
        }
        queueStep("Home/Reader reader to Home", SmokeStep::HomeReaderReturnedHome);
        break;

      case SmokeStep::HomeReaderReturnedHome: {
        if (!activityManager.isHomeActivity()) fail("Home/Reader from the reader did not return Home");
        Activity* const homeBeforeNoOp = activityManager.simulatorCurrentActivity();
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER) ||
            activityManager.simulatorCurrentActivity() != homeBeforeNoOp) {
          fail("Home/Reader was not a no-op on Home");
        }
        if (++homeReaderReaderKind < 3) {
          activityManager.replaceActivity(std::make_unique<HomeReaderSmokeActivity>(
              homeReaderSmokeReaderName(homeReaderReaderKind), true, renderer, mappedInputManager, true));
          queueStep("Home/Reader next reader type", SmokeStep::HomeReaderReader);
        } else {
          activityManager.simulatorCurrentActivity()->startActivityForResult(
              std::make_unique<HomeReaderSmokeActivity>("Settings", false, renderer, mappedInputManager),
              [](const ActivityResult&) {});
          queueStep("Home/Reader non-reader parent", SmokeStep::HomeReaderNonReaderParent);
        }
        break;
      }

      case SmokeStep::HomeReaderNonReaderParent:
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("Browser", false, renderer, mappedInputManager),
            [](const ActivityResult&) {});
        queueStep("Home/Reader non-reader nested", SmokeStep::HomeReaderNonReaderNested);
        break;

      case SmokeStep::HomeReaderNonReaderNested:
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER) ||
            !activityManager.handleShortcutAction(CrossPointSettings::HOME_READER)) {
          fail("Home/Reader outside a reader was not consumed");
        }
        queueStep("Home/Reader non-reader to Home", SmokeStep::HomeReaderNonReaderHome);
        break;

      case SmokeStep::HomeReaderNonReaderHome:
        if (!activityManager.isHomeActivity()) fail("Home/Reader from non-reader menus did not return Home");
        activityManager.replaceActivity(
            std::make_unique<HomeReaderSmokeActivity>("EpubReader", true, renderer, mappedInputManager, true));
        queueStep("Back/Home regression reader", SmokeStep::BackHomeReader);
        break;

      case SmokeStep::BackHomeReader:
        backHomeChildCancelled = false;
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("ReaderMenu", false, renderer, mappedInputManager),
            [this](const ActivityResult& result) { backHomeChildCancelled = result.isCancelled; });
        queueStep("Back/Home nested menu", SmokeStep::BackHomeNested);
        break;

      case SmokeStep::BackHomeNested:
        if (!activityManager.handleHomeButtonBackOrHome()) fail("Back/Home did not handle nested reader menu");
        queueStep("Back/Home returned one level", SmokeStep::BackHomeReturnedReader);
        break;

      case SmokeStep::BackHomeReturnedReader:
        if (!activityManager.simulatorCurrentActivity()->isReaderActivity() || !backHomeChildCancelled) {
          fail("Back/Home no longer pops exactly one canceled nested activity");
        }
        if (!activityManager.handleHomeButtonBackOrHome()) fail("Back/Home did not return from the reader");
        queueStep("Back/Home returned Home", SmokeStep::BackHomeReturnedHome);
        break;

      case SmokeStep::BackHomeReturnedHome:
        if (!activityManager.isHomeActivity()) fail("Back/Home from the reader did not return Home");
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
        prepareRecentLibrary();
        activityManager.goToLibrary();
        queueStep("Recent Library", SmokeStep::RecentLibrary);
        break;

      case SmokeStep::FileBrowserSettings:
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_SYNC")) {
          if (!mappedInputManager.hasHomeKey()) fail("Frontlight sync regression requires X4 Pro simulator");
          LOG_INF("SMOKE", "Frontlight sync: stats on/off menu and dispatch checks passed");
          step = SmokeStep::Done;
          break;
        }
        prepareRecentLibrary();
        activityManager.goToLibrary();
        queueStep("Recent Library", SmokeStep::RecentLibrary);
        break;

      case SmokeStep::RecentLibrary: {
        RenderLock lock;
        auto* activity = static_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
        if (Storage.exists(library::libraryIndexPath()) || !library::libraryIndexNeedsRefresh())
          fail("Recently Opened built the missing Library index");
        RecentBook book;
        if (activity->simulatorRowCount() != 18 || !activity->simulatorReadBook(0, book) ||
            book.path != "/books/recent-smoke-19.txt")
          fail("Recent Library did not show bounded history in newest-first order");
        activity->simulatorSetView(4, false);
        if (!activity->simulatorReadBook(0, book) || book.path != "/books/recent-smoke-2.txt")
          fail("Recent Library did not reverse history");
        activity->simulatorSetView(4, true, "Author 19");
        if (activity->simulatorRowCount() != 1 || !activity->simulatorReadBook(0, book) ||
            book.path != "/books/recent-smoke-19.txt")
          fail("Recent Library author search failed");
        SETTINGS.libraryShowTxt = 0;
        activity->simulatorSetView(4, true);
        if (activity->simulatorRowCount() != 0) fail("Recent Library file filter failed");
        SETTINGS.libraryShowTxt = 1;
        SETTINGS.libraryUseMetadata = 0;
        activity->simulatorSetView(4, true, "recent-smoke-19");
        if (activity->simulatorRowCount() != 1 || !activity->simulatorReadBook(0, book) ||
            book.title != "recent-smoke-19" || !book.author.empty())
          fail("Recent Library filename display/search failed");
        SETTINGS.libraryUseMetadata = 1;
        // An unreadable index must also be irrelevant to a history refresh.
        if (!Storage.writeFile(library::libraryIndexPath(), "broken")) fail("Cannot write broken index fixture");
        activity->simulatorSetView(4, true);
        activity->simulatorRefresh();
        FsFile broken;
        if (!Storage.openFileForRead("SMOKE", library::libraryIndexPath(), broken)) fail("Missing broken index");
        const auto brokenSize = broken.size();
        broken.close();
        if (brokenSize != 6 || !library::libraryIndexNeedsRefresh() || activity->simulatorRowCount() != 18)
          fail("Recent Library refreshed the full index");
        activity->simulatorSetView(1, false);
        activity->simulatorSetView(4, true);
        if (activity->simulatorRowCount() != 18 || !activity->simulatorReadBook(0, book))
          fail("Recent Library stayed unavailable after a failed full scan");
        const char* epubPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (epubPath) {
          const std::string cachePath = Epub(epubPath, "/.crosspoint").getCachePath();
          if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str()))
            fail("Cannot create completed-book cache");
          auto stats = BookReadingStats::load(cachePath);
          stats.isCompleted = true;
          if (!stats.save(cachePath)) fail("Cannot save completed-book fixture");
          RECENT_BOOKS.addOrUpdateBook(epubPath, "Completed smoke book", "", "");
          SETTINGS.libraryHideFinishedBooks = 1;
          activity->simulatorSetView(4, true);
          if (activity->simulatorRowCount() != 17) fail("Recent Library did not hide finished EPUB");
          SETTINGS.libraryHideFinishedBooks = 0;
          activity->simulatorSetView(4, true);
          if (activity->simulatorRowCount() != 18 || !activity->simulatorReadBook(0, book) || book.path != epubPath)
            fail("Recent Library did not restore finished EPUB");
          RECENT_BOOKS.removeByPath(epubPath);
          stats.isCompleted = false;
          if (!stats.save(cachePath)) fail("Cannot restore completed-book fixture");
        }
        for (int i = 0; i < 20; ++i) {
          const std::string path = "/books/recent-smoke-" + std::to_string(i) + ".txt";
          if (!Storage.remove(path.c_str())) fail("Cannot remove recent fixture");
        }
        activity->simulatorRefresh();
        if (activity->simulatorRowCount() != 0) fail("Recent Library did not prune missing books");
        // The builder deliberately retains an unreadable previous index. Remove
        // that fixture before checking a deferred build from a missing index.
        if (!Storage.remove(library::libraryIndexPath())) fail("Cannot remove broken index fixture");
        // Switching to a full-library sort must still build the deferred index.
        activity->simulatorSetView(1, false);
        if (library::libraryIndexNeedsRefresh() || activity->simulatorRowCount() == 0)
          fail("Full Library did not build its deferred index");
        SETTINGS.librarySortMethod = 1;
        SETTINGS.librarySortDescending = 0;
        LOG_INF("SMOKE",
                "Recent Library missing/corrupt index, 18-book limit, ordering, search, filters and completion passed");
        queueStep("Library", SmokeStep::Library);
        break;
      }

      case SmokeStep::Library: {
        // Rendering an error screen is not a successful Library smoke test.
        // The script supplies an isolated card with at least one EPUB.
        library::LibraryIndexFile shelf;
        const bool hasFixture = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK") != nullptr;
        const bool readable = shelf.open(library::libraryIndexPath());
        const bool populated = readable && (!hasFixture || shelf.bookCount() > 0);
        const uint16_t books = shelf.bookCount();
        shelf.close();
        if (!populated) fail("Library did not publish a readable populated index");
        constexpr char REFRESH_FIXTURE[] = "/books/library-refresh-smoke.txt";
        if (library::libraryIndexNeedsRefresh()) fail("Successful Library scan stayed dirty");
        if (libraryRefreshPass == 0) {
          libraryBaselineBooks = books;
          auto* libraryActivity = static_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
          const int beforeSelection = libraryActivity->simulatorSelection();
          {
            RenderLock busyRender;
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
            libraryActivity->loop();
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Up);
            libraryActivity->loop();
            if (libraryActivity->simulatorPendingInputs() != 2 ||
                libraryActivity->simulatorSelection() != beforeSelection)
              fail("Library did not buffer navigation while rendering");
#if CROSSINK_APP_CAP_TOUCH
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectTouchDown(200, 300);
            libraryActivity->loop();
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectTouchMove(200, 100);
            libraryActivity->loop();
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectTouchRelease(200, 100);
            libraryActivity->loop();
            if (libraryActivity->simulatorPendingInputs() != 5)
              fail("Library did not buffer touch press, cancellation and scroll");
#endif
          }
          mappedInputManager.simulatorClearInputFrame();
          while (libraryActivity->simulatorPendingInputs()) libraryActivity->loop();
          const int selectionBeforeOverflow = libraryActivity->simulatorSelection();
          {
            RenderLock busyRender;
            for (size_t i = 0; i <= LibraryInputBuffer::CAPACITY; ++i) {
              mappedInputManager.simulatorClearInputFrame();
              mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
              libraryActivity->loop();
            }
            if (libraryActivity->simulatorPendingInputs() != 1)
              fail("Library overflow did not cancel the partial input sequence");
          }
          mappedInputManager.simulatorClearInputFrame();
          libraryActivity->loop();
          if (libraryActivity->simulatorSelection() != selectionBeforeOverflow)
            fail("Library replayed navigation after input overflow");
          LOG_INF("SMOKE", "Library input buffering during rendering and overflow passed");
          // Deliberately bypass invalidation to prove that a normal return visit
          // reuses the index instead of walking the card again.
          if (!Storage.writeFile(REFRESH_FIXTURE, "Library refresh smoke fixture"))
            fail("Cannot create Library fixture");
        } else if (libraryRefreshPass == 1) {
          if (books != libraryBaselineBooks) fail("Library rescanned an unchanged session");
          library::invalidateLibraryIndex();
        } else if (libraryRefreshPass == 2) {
          if (books != libraryBaselineBooks + 1) fail("Library missed an invalidated addition");
          if (!Storage.remove(REFRESH_FIXTURE)) fail("Cannot delete Library fixture");
          library::invalidateLibraryIndex();
        } else if (books != libraryBaselineBooks) {
          fail("Library missed an invalidated deletion");
        }
        if (libraryRefreshPass++ < 3) {
          activityManager.goToLibrary();
          queueStep("Library cache reuse and invalidation", SmokeStep::Library);
          break;
        }
        LOG_INF("SMOKE", "Library reuse, addition and deletion refresh passed");
        if (mappedInputManager.hasHomeKey()) {
          renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
        }
        activityManager.goToSettings();
        queueStep(mappedInputManager.hasHomeKey() ? "Settings landscape" : "Settings", SmokeStep::Settings);
        break;
      }

      case SmokeStep::StatusBarEditor: {
        {
          RenderLock lock;
          captureStatusBarScreen("display-editor");
        }
        inputScript = {press(MappedInputManager::Button::Confirm), release(MappedInputManager::Button::Confirm),
                       render("Display status bar picker", 4)};
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::StatusBarPicker;
        step = SmokeStep::ReaderInput;
        break;
      }
      case SmokeStep::StatusBarPicker: {
        RenderLock lock;
        captureStatusBarScreen("display-picker");
        for (const uint8_t theme : {CrossPointSettings::CLASSIC, CrossPointSettings::MINIMAL, CrossPointSettings::LYRA,
                                    CrossPointSettings::ROUNDEDRAFF, CrossPointSettings::DASHBOARD}) {
          SETTINGS.uiTheme = theme;
          UITheme::getInstance().reload();
          const auto& metrics = UITheme::getInstance().getMetrics();
          for (unsigned format = 0; format < CrossPointSettings::DATE_FORMAT_COUNT; ++format) {
            SETTINGS.dateFormat = format;
            renderer.clearScreen();
            GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                           tr(STR_SETTINGS_TITLE));
            const auto name = "header-" + std::to_string(theme) + "-date-" + std::to_string(format);
            captureStatusBarScreen(name.c_str());
          }
        }
        LOG_INF("SMOKE", "Simulator smoke test passed: status bar editor, picker and themed date headers");
        std::_Exit(0);
      }

      case SmokeStep::Settings:
        renderer.setOrientation(GfxRenderer::Orientation::Portrait);
        if (!mappedInputManager.hasTouchHardware()) {
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::SideButtons;
          const auto down = mappedInputManager.menuButton(MappedInputManager::Button::Down);
          const auto up = mappedInputManager.menuButton(MappedInputManager::Button::Up);
          const auto left = mappedInputManager.menuButton(MappedInputManager::Button::Left);
          const auto right = mappedInputManager.menuButton(MappedInputManager::Button::Right);
          addTap(right);
          inputScript.push_back(render("Settings Right selects Reader tab", 3));
          inputScript.push_back(assertSettingsNavigation(1, 0));
          addTap(down);
          inputScript.push_back(render("Settings Down selects first row", 3));
          inputScript.push_back(assertSettingsNavigation(1, 1));
          addTap(left);
          inputScript.push_back(render("Settings Left switches tabs while a row is focused", 3));
          inputScript.push_back(assertSettingsNavigation(0, 1));
          addTap(up);
          inputScript.push_back(render("Settings Up returns to tab band", 3));
          inputScript.push_back(assertSettingsNavigation(0, 0));
          addTap(right);  // Reader tab
          addTap(right);  // Controls tab
          addTap(down);   // Power Button
          addTap(down);   // Front Buttons
          addTap(down);   // Side Buttons
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Side Button Settings", 250));
          step = SmokeStep::ReaderInput;
          break;
        }
        [[fallthrough]];
      case SmokeStep::SideButtons:
        activityManager.replaceActivity(std::make_unique<EpubReaderDrawerActivity>(
            renderer, mappedInputManager, std::shared_ptr<Epub>{}, nullptr, 0.0f, 0, 0, false, false, false, false,
            false, false, false, false, true, true, 0, 0, 5, false));
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
        verifyWakePowerReaderShortcut();
        buildReaderInputScript();
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::ReaderInput:
        runReaderInputScript();
        break;

      case SmokeStep::CarouselHome:
        verifyCarouselCacheReturn();
        break;

      case SmokeStep::FrontlightLayout: {
        // Both scales, all four orientations, and all eight themes, with and
        // without the additional reader progress row.
        if (frontlightLayoutPass == 128) {
          LOG_INF("SMOKE", "Frontlight layout: 128 scale/orientation/theme/context combinations passed");
          step = SmokeStep::Done;
          break;
        }
        {
          RenderLock lock;
          SETTINGS.uiScale =
              (frontlightLayoutPass / 64) ? CrossPointSettings::UI_SCALE_LARGE : CrossPointSettings::UI_SCALE_SMALL;
          SETTINGS.uiTheme = (frontlightLayoutPass / 8) % 8;
          renderer.setOrientation(static_cast<GfxRenderer::Orientation>((frontlightLayoutPass / 2) % 4));
          UITheme::getInstance().reload();
        }
        FrontlightPanelContext context;
        context.activeReaderBook = frontlightLayoutPass % 2;
        if (context.activeReaderBook) {
          context.bookTitle = "Layout fixture";
          context.bookDetails.title = context.bookTitle;
          context.bookDetails.chapterPage = 1;
          context.bookDetails.chapterPageCount = 10;
        }
        activityManager.pushActivity(
            std::make_unique<FrontlightPanelActivity>(renderer, mappedInputManager, std::move(context)));
        queueStep("Frontlight layout matrix", SmokeStep::FrontlightLayoutRendered, 4);
        break;
      }

      case SmokeStep::FrontlightLayoutRendered: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Layout matrix expected frontlight drawer");
        const auto handle = panel->simulatorHandleRect();
        int top, right, bottom, left;
        renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
        if (handle.y < top || handle.bottom() > renderer.getScreenHeight() - bottom || handle.x < left ||
            handle.right() > renderer.getScreenWidth() - right || handle.height <= 0 ||
            panel->simulatorContentBottom > panel->simulatorActionBarTop) {
          fail("Frontlight controls overflow in layout matrix case %u", frontlightLayoutPass);
        }
        if (const char* outputDir = std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_CAPTURES")) {
          const auto path = std::filesystem::path(outputDir) / (std::to_string(frontlightLayoutPass) + ".pgm");
          FILE* image = std::fopen(path.c_str(), "wb");
          if (!image) fail("Cannot create frontlight layout capture");
          RenderLock lock;
          const int width = renderer.getScreenWidth();
          const int height = renderer.getScreenHeight();
          std::fprintf(image, "P5\n%d %d\n255\n", width, height);
          for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
          }
          std::fclose(image);
        }
#if CROSSINK_APP_CAP_TOUCH
        const int x = handle.x + handle.width / 2;
        const int y = handle.y + handle.height / 2;
        inputScript = {touchDown(x, y), touchRelease(x, y), render("Frontlight closed by visible handle", 4),
                       assertActivity("Home")};
        scriptIndex = 0;
        ++frontlightLayoutPass;
        inputCompletionStep = SmokeStep::FrontlightLayout;
        step = SmokeStep::ReaderInput;
#else
        fail("Frontlight layout regression requires the X4 Pro simulator");
#endif
        break;
      }

      case SmokeStep::ThemeHome: {
#if CROSSINK_APP_CAP_TOUCH
        if (activityManager.getCurrentBookPath() != homeThemeBookPath) fail("Theme switch lost the selected book");
        const int width = renderer.getScreenWidth();
        const int height = renderer.getScreenHeight();
        inputScript = {touchDown(width / 2, 8),
                       touchMove(width / 2, height / 4),
                       touchRelease(width / 2, height / 4),
                       render("Home frontlight drawer", 4),
                       assertActivity("FrontlightPanel"),
                       {ScriptActionType::OpenFrontlightSettings, MappedInputManager::Button::Back, nullptr, 0, 0, 0},
                       render("Home drawer Settings", 4),
                       assertActivity("Settings")};
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::ThemeSettings;
        step = SmokeStep::ReaderInput;
#else
        fail("Home theme regression requires the X4 Pro simulator");
#endif
        break;
      }

      case SmokeStep::ThemeSettings: {
        static constexpr uint8_t themes[] = {CrossPointSettings::LYRA_3_COVERS, CrossPointSettings::DASHBOARD,
                                             CrossPointSettings::MINIMAL,       CrossPointSettings::COVER_GRID,
                                             CrossPointSettings::LYRA,          CrossPointSettings::CLASSIC,
                                             CrossPointSettings::ROUNDEDRAFF,   CrossPointSettings::LYRA_CAROUSEL,
                                             CrossPointSettings::LYRA_CAROUSEL, CrossPointSettings::LYRA_CAROUSEL};
        // Change the global values while the real Settings child is open, then
        // return through its real drawer callback. Compare with a fresh Home.
        {
          RenderLock lock;
          SETTINGS.uiTheme = themes[homeThemePass];
          if (homeThemePass == 8) SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
          UITheme::getInstance().reload();
        }
        inputScript = {press(MappedInputManager::Button::Back), release(MappedInputManager::Button::Back),
                       render("Home after drawer Settings", 8), assertActivity("Home")};
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::ThemeReturned;
        step = SmokeStep::ReaderInput;
        break;
      }

      case SmokeStep::ThemeReturned: {
        if (activityManager.getCurrentBookPath() != homeThemeBookPath) fail("Theme switch lost the selected book");
        {
          RenderLock lock;
          homeThemeScreenHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
        }
        activityManager.replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInputManager, HomeMenuItem::NONE,
                                                                       HalDisplay::FAST_REFRESH, homeThemeBookPath));
        queueStep("Fresh Home reference", SmokeStep::ThemeFresh, 8);
        break;
      }

      case SmokeStep::ThemeFresh: {
        {
          RenderLock lock;
          if (hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize()) != homeThemeScreenHash)
            fail("Home after drawer theme change differs from fresh Home (pass %u)", homeThemePass);
        }
        LOG_INF("SMOKE", "Home theme/scale return matches fresh render (pass %u)", homeThemePass);
        if (++homeThemePass == 10) {
          LOG_INF("SMOKE", "Simulator smoke test passed");
          std::_Exit(0);
        }
        step = SmokeStep::ThemeHome;
        break;
      }

      case SmokeStep::Done:
        if (SETTINGS.uiTheme == CrossPointSettings::LYRA_CAROUSEL && carouselCachePass == 0 &&
            std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK")) {
          const RecentBook book = RECENT_BOOKS.getBooks().front();
          for (const char* path : {"/books/carousel-second.txt", "/books/carousel-third.txt"}) {
            if (!Storage.writeFile(path, "Carousel side cover fixture")) fail("Cannot create carousel fixture");
            RECENT_BOOKS.addOrUpdateBook(path, path, {}, {}, RecentBook::CoverState::Missing);
          }
          RECENT_BOOKS.addOrUpdateBook(book.path, book.title, book.author, book.coverBmpPath, book.coverState);
          SETTINGS.trackReadingStats = 1;
          activityManager.goHome();
          queueStep("Carousel return cache", SmokeStep::CarouselHome, 8);
          break;
        }
        LOG_INF("SMOKE", "Simulator smoke test passed");
        std::_Exit(0);
    }
  }

  static ScriptAction assertReaderMenu(const ReaderDrawerTab tab, const ReaderDrawerPane pane, const int selected) {
    return {ScriptActionType::AssertReaderMenu,
            MappedInputManager::Button::Back,
            nullptr,
            selected,
            static_cast<int>(tab),
            static_cast<int>(pane)};
  }

  static ScriptAction assertSettingsNavigation(const int category, const int selected) {
    return {
        ScriptActionType::AssertSettingsNavigation, MappedInputManager::Button::Back, nullptr, 0, category, selected};
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

  static ScriptAction configureHomeButtonPowerLock() {
    return {ScriptActionType::ConfigureHomeButtonPowerLock, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction waitForPowerLongPress() {
    return {ScriptActionType::WaitForPowerLongPress, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertHomeButtonDisabled() {
    return {ScriptActionType::AssertHomeButtonDisabled, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertHomeButtonEnabled() {
    return {ScriptActionType::AssertHomeButtonEnabled, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenDisabled() {
    return {ScriptActionType::AssertTouchscreenDisabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenEnabled() {
    return {ScriptActionType::AssertTouchscreenEnabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTtfProfileNative() {
    return {ScriptActionType::AssertTtfProfileNative, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
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

  static ScriptAction assertActivity(const char* name) {
    return {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, name, 0, 0, 0};
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

#endif

  void addTap(MappedInputManager::Button button) {
    inputScript.push_back(press(button));
    inputScript.push_back(release(button));
  }

  void buildReaderInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::Done;

    int currentFontIndex = SETTINGS.fontFamily;
    std::vector<uint8_t> currentFontSizes(std::begin(BUILTIN_READER_FONT_SIZES), std::end(BUILTIN_READER_FONT_SIZES));
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      sdFontSystem.ensureRegistry();
      const auto& families = sdFontSystem.registry().getFamilies();
      for (size_t i = 0; i < families.size(); ++i) {
        if (families[i].name != SETTINGS.sdFontFamilyName) continue;
        currentFontIndex = CrossPointSettings::BUILTIN_FONT_COUNT + static_cast<int>(i);
        currentFontSizes = families[i].availableSizes();
        break;
      }
    }
    const auto sizeIt = std::find(currentFontSizes.begin(), currentFontSizes.end(), SETTINGS.readerFontPointSize);
    const int currentSizeIndex = sizeIt == currentFontSizes.end() ? 0 : std::distance(currentFontSizes.begin(), sizeIt);

    const int turns = pageTurnCount();
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouch()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      if (width <= 0 || height <= 0) fail("Touch smoke test has invalid screen dimensions");
      LOG_INF("SMOKE", "Running touch reader input script with %d page turn(s)", turns);
      const int tabY = height - 28;
      for (int i = 0; i < turns; ++i) {
        inputScript.push_back(touchDown(width * 5 / 6, height / 2));
        inputScript.push_back(touchRelease(width * 5 / 6, height / 2));
        inputScript.push_back(render("Reader after touch page forward", 4));
      }

      // Exercise the TTF edit path that replaces the active scalable font IDs:
      // Auto -> Native, switch tabs, then return to the current page.
#if CROSSINK_SCALABLE_FONTS
      if (SETTINGS.sdFontFamilyName[0] != '\0' && sdFontSystem.isScalableFamily(SETTINGS.sdFontFamilyName)) {
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader Menu opened for TTF Native transition", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2), tabY));
        inputScript.push_back(touchRelease(width / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2), tabY));
        addTap(MappedInputManager::Button::Confirm);
        addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("TTF Rendering opened in reader drawer", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("TTF Hinting choices opened in reader drawer", 3));
        const int currentHinting = TTF_RENDER_PROFILES.profileFor(SETTINGS.sdFontFamilyName).hinting;
        inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::EnumOptions, currentHinting));
        const auto hintingDirection = mappedInputManager.menuButton(
            currentHinting > 1 ? MappedInputManager::Button::Up : MappedInputManager::Button::Down);
        for (int i = 0; i < std::abs(currentHinting - 1); ++i) addTap(hintingDirection);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("TTF Native hinting selected", 4));
        inputScript.push_back(assertTtfProfileNative());
        inputScript.push_back(touchDown(width / 2, tabY));
        inputScript.push_back(touchRelease(width / 2, tabY));
        inputScript.push_back(render("Reader Menu tab changed after TTF Native selection", 5));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Reader restored after TTF Native selection", 10));
        inputScript.push_back(assertActivity("EpubReader"));
        addTap(MappedInputManager::Button::PageForward);
        inputScript.push_back(render("Reader page turn after TTF Native selection", 5));
      }
#endif

      if (mappedInputManager.hasHomeKey()) {
        // Reader long-Power actions fire at the hold threshold. Their release
        // must not reach main.cpp's global shortcut route and run the same
        // action again. Repeat the gesture to verify the consumed release does
        // not leave the next one latched.
        inputScript.push_back(configureHomeButtonPowerLock());
        inputScript.push_back(press(MappedInputManager::Button::Power));
        inputScript.push_back(waitForPowerLongPress());
        inputScript.push_back(assertHomeButtonDisabled());
        inputScript.push_back(release(MappedInputManager::Button::Power));
        inputScript.push_back(assertHomeButtonDisabled());
        inputScript.push_back(press(MappedInputManager::Button::Power));
        inputScript.push_back(waitForPowerLongPress());
        inputScript.push_back(assertHomeButtonEnabled());
        inputScript.push_back(release(MappedInputManager::Button::Power));
        inputScript.push_back(assertHomeButtonEnabled());

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
        // The book-progress row puts the portrait sheet's handle near 58% height.
        inputScript.push_back(touchDown(width / 2, height * 23 / 40));
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
        inputScript.push_back(touchDown(width * 7 / 10, height * 21 / 40));
        inputScript.push_back(touchRelease(width * 7 / 10, height * 21 / 40));
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
        inputScript.push_back(touchDown(width * 3 / 10, height * 21 / 40));
        inputScript.push_back(touchRelease(width * 3 / 10, height * 21 / 40));
        inputScript.push_back(render("Sync dialog opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height - 60));
        inputScript.push_back(touchRelease(width / 2, height - 60));
        inputScript.push_back(render("Reader restored after dismissing Frontlight sync dialog", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height / 2 + 24));
        inputScript.push_back(touchRelease(width / 2, height / 2 + 24));
        inputScript.push_back(render("Reader Font opened from touch reader menu", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader Menu root restored by simulated Home key tap", 8));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader restored by simulated Home key tap at drawer root", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu reopened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height / 2 - 14));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader restored after Reader Menu handle drag down", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(disableReaderTouch());
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from Home key hold with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Reader restored after Home key menu with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Home opened from simulated Home key tap", 8));
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
      inputScript.push_back(assertActivity("EpubReaderDrawer"));

      const int fontTabX = width / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2);
      inputScript.push_back(touchDown(fontTabX, tabY));
      inputScript.push_back(touchRelease(fontTabX, tabY));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Touch Reader Font choices", 3));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Touch Font Family opens on current choice", 4));
      inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::FontFamily, currentFontIndex));
      addTap(MappedInputManager::Button::Back);
      addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Touch Font Size opens on current choice", 4));
      inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::EnumOptions, currentSizeIndex));
      addTap(MappedInputManager::Button::Back);
      addTap(MappedInputManager::Button::Back);

      // Touch every bottom-drawer tab slot, then dismiss from its handle.
      for (int tab = 0; tab < static_cast<int>(READER_DRAWER_TAB_COUNT); ++tab) {
        const int tabX = width * (tab * 2 + 1) / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2);
        inputScript.push_back(touchDown(tabX, tabY));
        inputScript.push_back(touchRelease(tabX, tabY));
        inputScript.push_back(render("Touch Reader Menu tab", 3));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
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
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Auto Page Turn pane", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height - 8));
      inputScript.push_back(touchRelease(width / 2, height - 8));
      inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(width / 2, drawerTop - 14));
      inputScript.push_back(touchRelease(width / 2, drawerTop - 14));
      inputScript.push_back(render("Reader restored after drawer handle tap", 4));
      inputScript.push_back(assertActivity("EpubReader"));

      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Reader Menu reopened for bottom-edge Home gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height / 2 + 8));
      inputScript.push_back(touchRelease(width / 2, height / 2 + 8));
      inputScript.push_back(render("Reader Menu remains open after interior swipe up", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Home opened from Reader Menu bottom-edge swipe", 6));
      inputScript.push_back(assertActivity("Home"));
      return;
    }
#endif
    const auto menuDown = mappedInputManager.menuButton(MappedInputManager::Button::Down);
    const auto menuLeft = mappedInputManager.menuButton(MappedInputManager::Button::Left);
    const auto menuRight = mappedInputManager.menuButton(MappedInputManager::Button::Right);
    for (int i = 0; i < turns; i++) {
      addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Reader after page forward", 4));
    }

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu opened from EPUB", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    // Select retains its original tab-band behavior, including wraparound.
    for (const auto tab : {ReaderDrawerTab::Location, ReaderDrawerTab::Settings, ReaderDrawerTab::Font,
                           ReaderDrawerTab::Layout, ReaderDrawerTab::More}) {
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Reader Menu Select advances tab", 3));
      inputScript.push_back(assertReaderMenu(tab, ReaderDrawerPane::Root, 0));
    }
    addTap(menuLeft);
    addTap(menuLeft);
    inputScript.push_back(render("Reader Menu Font tab reached with Left", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 0));
    addTap(menuLeft);
    inputScript.push_back(render("Reader Menu wraps left to Settings", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Settings, ReaderDrawerPane::Root, 0));
    addTap(menuRight);
    inputScript.push_back(render("Reader Menu wraps right to Font", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 0));

    addTap(menuDown);
    inputScript.push_back(render("Reader Menu first row focused", 3));
    addTap(menuDown);
    inputScript.push_back(render("Reader Menu Down moves row focus", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 1));
    addTap(menuRight);
    inputScript.push_back(render("Reader Menu Right switches tab while a row is focused", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Layout, ReaderDrawerPane::Root, 0));
    addTap(menuLeft);
    addTap(menuDown);
    inputScript.push_back(render("Reader Menu first Font row focused again", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 0));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Font opened from Reader Menu", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::ReaderFont, 0));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Font Family opens on current choice", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::FontFamily, currentFontIndex));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Font after closing Font Family", 3));

    addTap(menuDown);
    inputScript.push_back(render("Font Size selected", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Font Size choices opened", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::EnumOptions, currentSizeIndex));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 font size paragraph preview", 4));
#endif

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Font after closing Font Size", 4));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 font family picker", 4));
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 font family paragraph preview", 4));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("C3 reader font restored", 3));
#endif

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Menu tab focus restored", 4));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(menuDown);
    addTap(menuDown);
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 spacing paragraph preview", 4));
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 line spacing adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 word spacing adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("C3 font tab restored", 3));
#endif

    addTap(menuRight);
    inputScript.push_back(render("Reader Menu advanced to next tab", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Layout, ReaderDrawerPane::Root, 0));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 margin paragraph preview", 4));
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 vertical margin adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 horizontal margin adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("C3 layout tab restored", 3));
#endif

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after closing Reader Menu", 4));
    inputScript.push_back(assertActivity("EpubReader"));
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu reopened on More", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader restored after checking default tab", 4));

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
      inputScript.push_back(
          {ScriptActionType::ClearFrontlightSyncBook, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight without last-read book", 4));
      inputScript.push_back({ScriptActionType::OpenFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Unavailable transfer actions rendered", 4));
      inputScript.push_back(
          {ScriptActionType::CheckFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      inputScript.push_back(press(MappedInputManager::Button::Confirm));
      inputScript.push_back(release(MappedInputManager::Button::Confirm));
      inputScript.push_back(render("Unavailable transfer action cannot activate", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(press(MappedInputManager::Button::Back));
      inputScript.push_back(release(MappedInputManager::Button::Back));
      inputScript.push_back(render("File Browser restored after unavailable transfer", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(
          {ScriptActionType::PrepareFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      for (const bool statsEnabled : {true, false}) {
        inputScript.push_back(
            {statsEnabled ? ScriptActionType::EnableReadingStats : ScriptActionType::DisableReadingStats,
             MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight sync from File Browser", 4));
        inputScript.push_back(
            {ScriptActionType::OpenFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        inputScript.push_back(render("Frontlight sync menu rendered", 4));
        inputScript.push_back(
            {ScriptActionType::CheckFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        inputScript.push_back(press(MappedInputManager::Button::Confirm));
        inputScript.push_back(release(MappedInputManager::Button::Confirm));
        inputScript.push_back(render("Frontlight sync opens account settings", 4));
        // This fork lists BookOrbit Sync first: its account screen is the one an unset sync opens.
        inputScript.push_back(assertActivity("BookOrbitSettings"));
        inputScript.push_back(press(MappedInputManager::Button::Back));
        inputScript.push_back(release(MappedInputManager::Button::Back));
        inputScript.push_back(render("File Browser restored after sync settings", 4));
        inputScript.push_back(assertActivity("FileBrowser"));
      }
      inputScript.push_back({ScriptActionType::EnableReadingStats, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
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
      case ScriptActionType::ClearFrontlightSyncBook:
        APP_STATE.openEpubPath.clear();
        break;
      case ScriptActionType::PrepareFrontlightSync: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (!bookPath || !Storage.exists(bookPath)) fail("Frontlight sync fixture is missing");
        APP_STATE.openEpubPath = bookPath;
        break;
      }
      case ScriptActionType::DisableReadingStats:
        SETTINGS.trackReadingStats = 0;
        break;
      case ScriptActionType::EnableReadingStats:
        SETTINGS.trackReadingStats = 1;
        break;
      case ScriptActionType::OpenFrontlightSync: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Expected frontlight drawer before opening sync menu");
        panel->simulatorActivateQuickAction(1);
        break;
      }
      case ScriptActionType::CheckFrontlightSync: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Expected frontlight sync menu");
        const bool unavailable = action.x != 0;
        for (int index = 0; index < 3; ++index) {
          if (panel->simulatorSyncOptionDisabled(index) != unavailable)
            fail("Unexpected transfer availability outside reader");
          const auto row = panel->simulatorSyncOptionRect(index);
          // Sample inside the row background, away from its centered label and rounded corners.
          if (unavailable || index > 0) {
            for (int offset = 0; offset < 8; ++offset)
              if (renderer.isPixelBlack(row.x + row.width / 2 + offset, row.y + 4))
                fail("Unselected sync option is highlighted");
          }
        }
        if (const char* outputDir = std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_CAPTURES")) {
          const auto path = std::filesystem::path(outputDir) / (unavailable                  ? "sync-no-book.pgm"
                                                                : SETTINGS.trackReadingStats ? "sync-stats-on.pgm"
                                                                                             : "sync-stats-off.pgm");
          FILE* image = std::fopen(path.c_str(), "wb");
          if (!image) fail("Cannot create sync menu capture");
          RenderLock lock;
          const int width = renderer.getScreenWidth();
          const int height = renderer.getScreenHeight();
          std::fprintf(image, "P5\n%d %d\n255\n", width, height);
          for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
          std::fclose(image);
        }
        break;
      }
      case ScriptActionType::OpenFrontlightSettings: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Expected frontlight drawer before opening Settings");
        panel->simulatorActivateQuickAction(3);
        break;
      }
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
      case ScriptActionType::ConfigureHomeButtonPowerLock:
        SETTINGS.homeButtonInReaderEnabled = 1;
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER;
        SETTINGS.longPwrBtn = CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER;
        break;
      case ScriptActionType::WaitForPowerLongPress:
        if (mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
          --scriptIndex;
        }
        break;
      case ScriptActionType::AssertHomeButtonDisabled:
        if (SETTINGS.homeButtonInReaderEnabled) fail("Long Power did not disable the Home button");
        break;
      case ScriptActionType::AssertHomeButtonEnabled:
        if (!SETTINGS.homeButtonInReaderEnabled) fail("Long Power did not enable the Home button");
        break;
      case ScriptActionType::AssertTouchscreenDisabled:
        if (!SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be disabled");
        break;
      case ScriptActionType::AssertTouchscreenEnabled:
        if (SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be enabled");
        break;
      case ScriptActionType::AssertTtfProfileNative:
#if CROSSINK_SCALABLE_FONTS
        if (TTF_RENDER_PROFILES.profileFor(SETTINGS.sdFontFamilyName).hinting != 1) {
          fail("Expected active TTF profile to use native hinting");
        }
#endif
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
      case ScriptActionType::AssertReaderMenu: {
        if (!activityManager.isCurrentActivityNamed("EpubReaderDrawer"))
          fail("Expected reader menu for navigation assertion");
        const auto* drawer = static_cast<EpubReaderDrawerActivity*>(activityManager.simulatorCurrentActivity());
        const auto& state = drawer->simulatorState();
        if ((state.pane == ReaderDrawerPane::FontFamily || state.pane == ReaderDrawerPane::EnumOptions) &&
            !drawer->simulatorFocusedRowVisible())
          fail("Reader picker current choice is not highlighted and visible");
        if (static_cast<int>(state.tab) != action.x || static_cast<int>(state.pane) != action.y ||
            state.selectedIndex != action.settleFrames)
          fail("Reader menu navigation mismatch: tab=%d pane=%d row=%d, expected %d/%d/%d", static_cast<int>(state.tab),
               static_cast<int>(state.pane), state.selectedIndex, action.x, action.y, action.settleFrames);
        break;
      }
      case ScriptActionType::AssertSettingsNavigation: {
        if (!activityManager.isCurrentActivityNamed("Settings")) fail("Expected settings for navigation assertion");
        const auto* settings = static_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
        if (settings->simulatorCategoryIndex() != action.x || settings->simulatorSelectedIndex() != action.y)
          fail("Settings navigation mismatch: category=%d row=%d, expected %d/%d", settings->simulatorCategoryIndex(),
               settings->simulatorSelectedIndex(), action.x, action.y);
        break;
      }
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
