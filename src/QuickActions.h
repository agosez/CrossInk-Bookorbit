#pragma once

#include <CrossInkHalFrontlight.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <array>
#include <functional>

#include "CrossPointSettings.h"

class OptionPopup;

// One source of truth for the shortcut that opens Quick Actions.  UI and web
// settings call this after changing an action, so the persisted state cannot
// end up with two physical gestures claiming the same menu.
namespace QuickActions {
// Keep existing values stable because the selected trigger is persisted.
enum class Trigger : uint8_t {
  None = 0,
  ShortPower,
  LongPower,
  LongBack,
  LongMenu,
  PowerUp,
  TapHome,
  LongPressHome,
  DoubleTapHome,
  UpDown,
};

inline constexpr std::array<StrId, CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT> actionLabels = {
    StrId::STR_IGNORE,
    StrId::STR_SLEEP,
    StrId::STR_PAGE_TURN,
    StrId::STR_FORCE_REFRESH,
    StrId::STR_CHANGE_FONT,
    StrId::STR_TOGGLE_GUIDE_DOTS,
    StrId::STR_TOGGLE_BIONIC_READING,
    StrId::STR_TOGGLE_BOOKMARK,
    StrId::STR_SYNC_PROGRESS,
    StrId::STR_MARK_FINISHED,
    StrId::STR_READING_STATS,
    StrId::STR_SCREENSHOT_BUTTON,
    StrId::STR_CYCLE_PAGE_TURN,
    StrId::STR_FILE_TRANSFER,
    StrId::STR_TILT_PAGE_TURN,
    StrId::STR_READER_DARK_MODE,
    StrId::STR_FOOTNOTES,
    StrId::STR_BROWSE_FILES,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CREATE_HOTSPOT,
    StrId::STR_SAVE_CLIPPING,
    StrId::STR_LOOKUP};

// Shared display order for shortcut pickers. The values remain the persisted
// SHORT_PWRBTN IDs; only their presentation order is centralized here.
inline constexpr std::array<CrossPointSettings::SHORT_PWRBTN, 31> shortcutActionOrder = {
    CrossPointSettings::IGNORE,
    CrossPointSettings::SLEEP,
    CrossPointSettings::PAGE_TURN,
    CrossPointSettings::PREVIOUS_PAGE,
    CrossPointSettings::TOGGLE_BOOKMARK,
    CrossPointSettings::READING_STATS,
    CrossPointSettings::MARK_FINISHED,
    CrossPointSettings::FORCE_REFRESH,
    CrossPointSettings::TOGGLE_FONT,
    CrossPointSettings::TOGGLE_GUIDE_DOTS,
    CrossPointSettings::TOGGLE_BIONIC_READING,
    CrossPointSettings::CYCLE_PAGE_TURN,
    CrossPointSettings::TOGGLE_TILT_PAGE_TURN,
    CrossPointSettings::SYNC_PROGRESS,
    CrossPointSettings::BOOKORBIT_SYNC,
    CrossPointSettings::NEARBY_POSITION_SYNC,
    CrossPointSettings::FILE_TRANSFER,
    CrossPointSettings::CALIBRE_WIRELESS,
    CrossPointSettings::JOIN_NETWORK,
    CrossPointSettings::CREATE_HOTSPOT,
    CrossPointSettings::SCREENSHOT,
    CrossPointSettings::TOGGLE_DARK_MODE,
    CrossPointSettings::FOOTNOTES,
    CrossPointSettings::FILE_BROWSER,
    CrossPointSettings::CREATE_CLIPPING,
    CrossPointSettings::LOOKUP_WORD,
    CrossPointSettings::TOGGLE_HOME_BUTTON_IN_READER,
    CrossPointSettings::QUICK_ACTIONS,
    CrossPointSettings::QUICK_LOCK,
    CrossPointSettings::TOGGLE_FRONTLIGHT,
    CrossPointSettings::TOGGLE_TOUCHSCREEN,
};

inline bool supportsTiltPageTurn() { return halTiltSensor.isAvailable(); }

inline bool isActionAvailable(const uint8_t action) {
#if !CROSSINK_APP_CAP_KOREADER_SYNC
  // The X3/X4 builds of this fork ship without the KOReader sync screens, so no
  // picker may offer the action. Its persisted number is untouched: a settings
  // file still holding it simply finds it unavailable, exactly as a frontlight
  // toggle is unavailable on a board with no frontlight.
  if (action == CrossPointSettings::SYNC_PROGRESS) return false;
#endif
  if (action == CrossPointSettings::PREVIOUS_PAGE || action == CrossPointSettings::NEARBY_POSITION_SYNC) return true;
  if (action == CrossPointSettings::QUICK_ACTIONS || action == CrossPointSettings::QUICK_LOCK) return true;
  if (action == CrossPointSettings::BOOKORBIT_SYNC) return true;
  if (action == CrossPointSettings::TOGGLE_FRONTLIGHT) return Frontlight.present();
  if (action == CrossPointSettings::TOGGLE_TOUCHSCREEN) return gpio.hasTouch();
  if (action < CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT) {
    return action != CrossPointSettings::TOGGLE_TILT_PAGE_TURN || supportsTiltPageTurn();
  }
  return action == CrossPointSettings::TOGGLE_HOME_BUTTON_IN_READER && gpio.hasHomeKey();
}

// Quick Lock needs a single physical shortcut to unlock. Quick Actions opens
// this same menu, so neither belongs in a menu slot.
inline bool isQuickActionSlotActionAvailable(const uint8_t action) {
  return action != CrossPointSettings::QUICK_LOCK && action != CrossPointSettings::QUICK_ACTIONS &&
         isActionAvailable(action);
}

inline StrId actionLabel(const uint8_t action) {
  // STR_PAGE_TURN also labels the touch gesture setting, so shortcuts use the
  // directional label without changing that setting's wording.
  if (action == CrossPointSettings::PAGE_TURN) return StrId::STR_NEXT_PAGE;
  if (action < CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT) return actionLabels[action];
  if (action == CrossPointSettings::QUICK_ACTIONS) return StrId::STR_QUICK_ACTIONS;
  if (action == CrossPointSettings::TOGGLE_FRONTLIGHT) return StrId::STR_TOGGLE_FRONTLIGHT;
  if (action == CrossPointSettings::TOGGLE_TOUCHSCREEN) return StrId::STR_TOGGLE_TOUCHSCREEN;
  if (action == CrossPointSettings::QUICK_LOCK) return StrId::STR_QUICK_LOCK;
  if (action == CrossPointSettings::BOOKORBIT_SYNC) return StrId::STR_BOOKORBIT_SYNC;
  if (action == CrossPointSettings::PREVIOUS_PAGE) return StrId::STR_PREV_PAGE;
  if (action == CrossPointSettings::NEARBY_POSITION_SYNC) return StrId::STR_NEARBY_POSITION_SYNC;
  return StrId::STR_HOME_BUTTON_LOCK;
}

inline void synchronize(CrossPointSettings& settings, Trigger preferred = Trigger::None) {
  const bool shortPower = settings.shortPwrBtn == CrossPointSettings::QUICK_ACTIONS;
  const bool longPower = settings.longPwrBtn == CrossPointSettings::QUICK_ACTIONS;
  const bool longBack = settings.longPressBackAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  const bool longMenu = settings.longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  const bool powerUp = settings.powerChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS;
  const bool upDown = settings.sideButtonChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS;
  const bool tapHome = settings.homeButtonTapAction == CrossPointSettings::QUICK_ACTIONS;
  const bool longPressHome = settings.homeButtonLongPressAction == CrossPointSettings::QUICK_ACTIONS;
  const bool doubleTapHome = settings.homeButtonDoubleTapAction == CrossPointSettings::QUICK_ACTIONS;

  Trigger owner = preferred;
  if (owner == Trigger::None) {
    if (shortPower)
      owner = Trigger::ShortPower;
    else if (longPower)
      owner = Trigger::LongPower;
    else if (longBack)
      owner = Trigger::LongBack;
    else if (longMenu)
      owner = Trigger::LongMenu;
    else if (powerUp)
      owner = Trigger::PowerUp;
    else if (upDown)
      owner = Trigger::UpDown;
    else if (tapHome)
      owner = Trigger::TapHome;
    else if (longPressHome)
      owner = Trigger::LongPressHome;
    else if (doubleTapHome)
      owner = Trigger::DoubleTapHome;
  }

  if (owner != Trigger::ShortPower && shortPower) settings.shortPwrBtn = CrossPointSettings::IGNORE;
  if (owner != Trigger::LongPower && longPower) settings.longPwrBtn = CrossPointSettings::IGNORE;
  if (owner != Trigger::LongBack && longBack) settings.longPressBackAction = CrossPointSettings::LONG_MENU_OFF;
  if (owner != Trigger::LongMenu && longMenu) settings.longPressMenuAction = CrossPointSettings::LONG_MENU_OFF;
  if (owner != Trigger::PowerUp && powerUp) settings.powerChordAction = CrossPointSettings::CHORD_DISABLED;
  if (owner != Trigger::UpDown && upDown) settings.sideButtonChordAction = CrossPointSettings::CHORD_DISABLED;
  if (owner != Trigger::TapHome && tapHome) settings.homeButtonTapAction = CrossPointSettings::HOME_BUTTON_BACK_HOME;
  if (owner != Trigger::LongPressHome && longPressHome) {
    settings.homeButtonLongPressAction = CrossPointSettings::HOME_BUTTON_READER_MENU;
  }
  if (owner != Trigger::DoubleTapHome && doubleTapHome) {
    settings.homeButtonDoubleTapAction = CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT;
  }
  settings.quickActionsTrigger = static_cast<uint8_t>(owner);
}

inline void applyTrigger(CrossPointSettings& settings, const Trigger trigger) {
  if (settings.shortPwrBtn == CrossPointSettings::QUICK_ACTIONS) settings.shortPwrBtn = CrossPointSettings::IGNORE;
  if (settings.longPwrBtn == CrossPointSettings::QUICK_ACTIONS) settings.longPwrBtn = CrossPointSettings::IGNORE;
  if (settings.longPressBackAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS) {
    settings.longPressBackAction = CrossPointSettings::LONG_MENU_OFF;
  }
  if (settings.longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS) {
    settings.longPressMenuAction = CrossPointSettings::LONG_MENU_OFF;
  }
  if (settings.powerChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS) {
    settings.powerChordAction = CrossPointSettings::CHORD_DISABLED;
  }
  if (settings.sideButtonChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS) {
    settings.sideButtonChordAction = CrossPointSettings::CHORD_DISABLED;
  }
  if (settings.homeButtonTapAction == CrossPointSettings::QUICK_ACTIONS) {
    settings.homeButtonTapAction = CrossPointSettings::HOME_BUTTON_BACK_HOME;
  }
  if (settings.homeButtonLongPressAction == CrossPointSettings::QUICK_ACTIONS) {
    settings.homeButtonLongPressAction = CrossPointSettings::HOME_BUTTON_READER_MENU;
  }
  if (settings.homeButtonDoubleTapAction == CrossPointSettings::QUICK_ACTIONS) {
    settings.homeButtonDoubleTapAction = CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT;
  }

  if (trigger == Trigger::ShortPower) settings.shortPwrBtn = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::LongPower) settings.longPwrBtn = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::LongBack) settings.longPressBackAction = CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  if (trigger == Trigger::LongMenu) settings.longPressMenuAction = CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  if (trigger == Trigger::PowerUp) settings.powerChordAction = CrossPointSettings::CHORD_QUICK_ACTIONS;
  if (trigger == Trigger::UpDown) settings.sideButtonChordAction = CrossPointSettings::CHORD_QUICK_ACTIONS;
  if (trigger == Trigger::TapHome) settings.homeButtonTapAction = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::LongPressHome) settings.homeButtonLongPressAction = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::DoubleTapHome) settings.homeButtonDoubleTapAction = CrossPointSettings::QUICK_ACTIONS;
  settings.quickActionsTrigger = static_cast<uint8_t>(trigger);
}

inline Trigger triggerForSetting(uint8_t CrossPointSettings::* member) {
  if (member == &CrossPointSettings::shortPwrBtn) return Trigger::ShortPower;
  if (member == &CrossPointSettings::longPwrBtn) return Trigger::LongPower;
  if (member == &CrossPointSettings::longPressBackAction) return Trigger::LongBack;
  if (member == &CrossPointSettings::longPressMenuAction) return Trigger::LongMenu;
  if (member == &CrossPointSettings::powerChordAction) return Trigger::PowerUp;
  if (member == &CrossPointSettings::sideButtonChordAction) return Trigger::UpDown;
  if (member == &CrossPointSettings::homeButtonTapAction) return Trigger::TapHome;
  if (member == &CrossPointSettings::homeButtonLongPressAction) return Trigger::LongPressHome;
  if (member == &CrossPointSettings::homeButtonDoubleTapAction) return Trigger::DoubleTapHome;
  return Trigger::None;
}

inline void settingChanged(CrossPointSettings& settings, uint8_t CrossPointSettings::* member) {
  const Trigger trigger = triggerForSetting(member);
  if (trigger == Trigger::None) return;
  const bool selected = trigger == Trigger::LongBack || trigger == Trigger::LongMenu
                            ? settings.*member == CrossPointSettings::LONG_MENU_QUICK_ACTIONS
                        : (trigger == Trigger::PowerUp || trigger == Trigger::UpDown)
                            ? settings.*member == CrossPointSettings::CHORD_QUICK_ACTIONS
                            : settings.*member == CrossPointSettings::QUICK_ACTIONS;
  synchronize(settings, selected ? trigger : Trigger::None);
}

using ActionHandler = std::function<void(CrossPointSettings::SHORT_PWRBTN)>;
using ActionFilter = std::function<bool(CrossPointSettings::SHORT_PWRBTN)>;

void showConfiguredPopup(OptionPopup& popup, const std::function<void()>& requestUpdate,
                         ActionHandler actionHandler = {}, ActionFilter actionFilter = {});
}  // namespace QuickActions
