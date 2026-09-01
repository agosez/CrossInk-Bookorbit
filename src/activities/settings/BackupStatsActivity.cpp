#include "BackupStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/reader/StatsBackup.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
TouchActionButtons::Layout backupActions(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  constexpr uint8_t buttonCount = 2;
  constexpr int totalHeight =
      TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap * (buttonCount - 1);
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}
}  // namespace

void BackupStatsActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  backupFileName[0] = '\0';
  requestUpdate();
}

void BackupStatsActivity::onExit() { Activity::onExit(); }

void BackupStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BACKUP_NOW), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BACKUP_NOW));
  }

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BACKUP_STATS_CONFIRM), true);

    if (mappedInput.hasTouchHardware()) {
      const auto actions = backupActions(renderer, metrics);
      const char* actionLabels[] = {tr(STR_CONFIRM), tr(STR_CANCEL)};
      TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BACKUP_STATS_DONE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, backupFileName[0] != '\0' ? backupFileName : "-");

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BACKUP_STATS_FAILED), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BackupStatsActivity::runBackup() {
  state = backupGlobalStats(true, backupFileName, sizeof(backupFileName)) ? SUCCESS : FAILED;
  requestUpdate();
}

void BackupStatsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    goBack();
    return;
  }
  if (state == WARNING) {
    if (mappedInput.hasTouchHardware()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const auto actions = backupActions(renderer, metrics);
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(
          touchedOption, actions.buttons[0].y, TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
          actions.count, actions.buttons[0].x, actions.buttons[0].x + actions.buttons[0].width,
          actions.buttons[0].height);
      if (touch == MappedInputManager::RowTouch::Down) return;
      if (touch == MappedInputManager::RowTouch::Tap) {
        if (touchedOption == 0) {
          runBackup();
        } else if (touchedOption == 1) {
          goBack();
        }
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      runBackup();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
  }
}
