#pragma once

#include <functional>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

enum class NetworkMode {
  JOIN_NETWORK,
  CONNECT_CALIBRE,
  CREATE_HOTSPOT,
  USB_DRIVE,
  NEARBY_BOOK_RECEIVE,
  NEARBY_STATS_SYNC
};

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "Sync Stats" - Sync reading stats directly with a nearby reader
 * - "Receive File" - Receive a file directly from another reader
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  // FreeInkApp hosts the mode list (themed rows, icons, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiHost = UiAppHost<12, 4>;
  using UiApp = UiHost::App;

  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

  UiHost ui;
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  freeink::ui::ListNav listNav;
  ScreenTransitionRefresh screenTransitionRefresh;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void selectCurrent();

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
