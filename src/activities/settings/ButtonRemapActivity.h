#pragma once

#include <array>
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

class ButtonRemapActivity final : public Activity {
 public:
  // isReaderMode = true  → saves to reader-specific front button fields
  // isReaderMode = false → saves to system-wide front button fields (default)
  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool isReaderMode = false,
                               bool headerReaderContext = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiHost = UiAppHost<4, 1>;
  using UiApp = UiHost::App;

  bool readerMode;
  bool headerReaderContext;

  // Index of the logical role currently awaiting input.
  uint8_t currentStep = 0;
  // Temporary mapping from logical role -> hardware button index.
  uint8_t tempMapping[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  // Error banner timing (used when reassigning duplicate buttons).
  unsigned long errorUntil = 0;
  std::string errorMessage;
  static constexpr size_t ROLE_NAME_BUFFER_SIZE = 128;
  mutable std::array<char, ROLE_NAME_BUFFER_SIZE> roleNameStorage{};
  UiHost ui;
  std::array<freeink::ui::ListItem, 4> listItems{};

  static void listScreen(UiApp::ScreenType& screen, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void refreshListItems();
  bool usesLyraValueBadge() const;
  void drawSelectedValueBadge(UiApp::ScreenType& screen, freeink::ui::Rect listRect,
                              const freeink::ui::ListProps& props) const;
  // Commit temporary mapping to settings.
  void applyTempMapping();
  // Returns false if a hardware button is already assigned to a different role.
  bool validateUnassigned(uint8_t pressedButton);
  // Labels for UI display.
  const char* getRoleName(uint8_t roleIndex) const;
  const char* getHardwareName(uint8_t buttonIndex) const;
};
