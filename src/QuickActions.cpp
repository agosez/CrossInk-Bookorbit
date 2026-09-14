#include "QuickActions.h"

#include <I18n.h>

#include <vector>

#include "GlobalActions.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

namespace QuickActions {
namespace {

bool sleepScreenNeedsUnderlyingFrame() {
  // These modes deliberately keep some or all of the currently displayed
  // framebuffer. All other sleep screens paint an opaque replacement.
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME;
}

}  // namespace

void showConfiguredPopup(OptionPopup& popup, const std::function<void()>& requestUpdate, ActionHandler actionHandler,
                         ActionFilter actionFilter) {
  std::vector<std::string> labels;
  std::vector<uint8_t> actions;
  labels.reserve(std::size(SETTINGS.quickActionSlots));
  actions.reserve(std::size(SETTINGS.quickActionSlots));
  for (const uint8_t action : SETTINGS.quickActionSlots) {
    const auto shortcutAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(action);
    if (action == CrossPointSettings::IGNORE || !isQuickActionSlotActionAvailable(action) ||
        (actionFilter && !actionFilter(shortcutAction))) {
      continue;
    }
    labels.emplace_back(I18N.get(actionLabel(action)));
    actions.push_back(action);
  }
  if (actions.empty()) return;
  popup.show(StrId::STR_QUICK_ACTIONS, labels, 0,
             [actions = std::move(actions), actionHandler = std::move(actionHandler)](const int selected) {
               if (selected >= 0 && static_cast<size_t>(selected) < actions.size()) {
                 const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(actions[selected]);
                 // These actions read or write the current framebuffer immediately.
                 // Render the underlying screen after dismissing the popup so none of
                 // them captures or paints over the stale modal image. Other actions
                 // already schedule their own redraw and should not pay for an extra
                 // full-page render here.
                 // Quick Lock is never offered here (isQuickActionSlotActionAvailable
                 // filters it out) because unlocking needs a single physical shortcut.
                 if ((action == CrossPointSettings::SHORT_PWRBTN::SLEEP && sleepScreenNeedsUnderlyingFrame()) ||
                     (action == CrossPointSettings::SHORT_PWRBTN::SCREENSHOT &&
                      !activityManager.canSnapshotForSleepOverlay())) {
                   (void)activityManager.requestUpdateAndWait();
                 }
                 if (actionHandler) {
                   actionHandler(action);
                 } else {
                   dispatchShortcutAction(action);
                 }
               }
             });
  requestUpdate();
}
}  // namespace QuickActions
