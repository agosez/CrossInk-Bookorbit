#pragma once
#include <cstdint>
#include <string>
#include <utility>

#include "activities/Activity.h"

class Bitmap;

// The BookOrbit sleep sync draws the sleep screen twice: before the sync, and again once
// it is over, because the screen loses contrast while the sync runs behind it. The first
// pass records what the second needs to come out the same: the random image it picked,
// and the screen a Page Overlay was drawn over. The second pass has no popup.
enum class SleepScreenPass : uint8_t { Normal, RecordForRedraw, Redraw };

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         std::string currentBookPath = {}, bool fromTimeout = false,
                         GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait,
                         SleepScreenPass pass = SleepScreenPass::Normal)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        currentBookPath(std::move(currentBookPath)),
        fromTimeout(fromTimeout),
        sleepPopupOrientation(sleepPopupOrientation),
        pass(pass) {}
  void onEnter() override;

 private:
  void renderSleepScreen();
  bool rendersQuickResume() const;
  void recordOverlayBackground() const;
  void recordRandomImage() const;
  void loadRedrawRecord();
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderMinimalSleepScreen() const;
  void renderMinimalStatsSleepScreen() const;
  void renderDashboardSleepScreen() const;
  bool renderBitmapSleepScreen(Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  bool canSnapshotOverlayBackground = false;
  bool overlayBackgroundBufferStored = false;
  // Redraw only: the recorded overlay background is already in the frame buffer.
  bool overlayBackgroundLoaded = false;
  std::string currentBookPath;
  bool fromTimeout = false;
  GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait;
  SleepScreenPass pass = SleepScreenPass::Normal;
};
