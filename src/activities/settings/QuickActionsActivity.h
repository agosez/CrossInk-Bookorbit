#pragma once

#include <array>

#include "QuickActions.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class QuickActionsActivity final : public Activity {
  OptionPopup popup;
  QuickActions::Trigger draftTrigger = QuickActions::Trigger::None;
  std::array<uint8_t, 5> draftSlots{};
  void showOverview();
  void editShortcut();
  void editSlot(uint8_t slot);
  void saveDraft();

 public:
  QuickActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("QuickActions", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#ifdef SIMULATOR
namespace QuickActionsActivityTest {
bool isTriggerAvailable(QuickActions::Trigger trigger);
}
#endif
