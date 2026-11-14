#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "BookOrbitHomeShelfSource.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for BookOrbit Sync settings.
 * Shows username, password, server URL, and an authenticate option.
 *
 * Unlike KOReaderSettingsActivity there is no document-matching toggle: BookOrbit
 * always identifies documents by the binary partial-MD5 hash.
 */
class BookOrbitSettingsActivity final : public Activity {
 public:
  // Main is BookOrbit Sync itself. Home is its Home row, a page of its own: what the
  // BookOrbit theme's two Library shelves list, each picked in one popup from Discover,
  // Recently Added, Want to Read and the collections and SmartScopes its last refresh
  // saved to the SD card. The refresh runs in its own network
  // boot (NetworkBootTarget::BOOKORBIT_HOME_SHELVES), which opens the Home page alone with
  // networkBoot set: it fetches first, turns Wi-Fi off, and leaving restarts to Home.
  // Leaving the Home page with shelves the Home snapshot does not show yet syncs the Home
  // under the BookOrbit theme, so they are there when it opens. One class for both pages
  // keeps the C3 from paying for a second list screen.
  enum class Page : uint8_t { Main, Home };

  explicit BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Page page = Page::Main,
                                     bool networkBoot = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return fetching; }

 private:
  // FreeInkApp hosts the settings list (themed rows, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<12, 4>;

  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void handleSelection();
  int rowCount() const;

  // The Home page.
  void handleHomeSelection();
  void showShelfSources(int shelf);
  void chooseShelf(int shelf, const BookOrbitHomeShelfSource& source);
  void fetchShelfOptions();
  void leaveHome();

  const Page page;
  const bool networkBoot;
  // The Home page's pickers, made in onEnter(): Main has none, and on the heap the popup
  // costs the C3 no constructor of its own.
  std::unique_ptr<OptionPopup> optionPopup;
  // What the last refresh saved, 67 bytes each and at most 64, while the Home page is
  // open: the name popup lists them. Changed under the render lock.
  std::vector<BookOrbitHomeShelfSource> shelfOptions;
  bool hasShelfOptions = false;  // a refresh has saved a list, possibly empty
  bool fetching = false;         // the network boot's refresh, under its popup
  bool fetchPending = false;
  bool fetchOnline = false;  // Wi-Fi came up for the network boot's refresh
  bool fetchFailed = false;
  unsigned long failedAt = 0;
};
