#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/ButtonNavigator.h"

// Reader status bar configuration activity
class StatusBarSettingsActivity final : public Activity {
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool readerContext = false,
                                     bool stablePageNumbersAvailable = false)
      : Activity("StatusBarSettings", renderer, mappedInput),
        readerContext(readerContext),
        stablePageNumbersAvailable(stablePageNumbersAvailable),
        uiTarget(makeUiTarget(renderer)),
        app(uiTarget, uiTarget.deviceContext()) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;

  int selectedIndex = 0;
  int visibleItemCount = 0;
  bool readerContext = false;
  bool stablePageNumbersAvailable = false;

  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  freeink::ui::GfxRendererTarget uiTarget;  // Must precede app: the app holds a reference to it.
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  int itemForVisibleIndex(int visibleIndex) const;
  bool selectedItemUsesOptionMenu() const;
  void handleSelection();
  void openOptionPicker();
  static void settingsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildSettingsScreen(UiApp::ScreenType& screen);
};
