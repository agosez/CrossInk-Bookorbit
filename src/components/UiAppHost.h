#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "UiAppHelpers.h"

class GfxRenderer;
class MappedInputManager;

// Owns the small protocol shared by FreeInkUI activities.  Capacity remains a
// property of the activity so a simple screen does not pay the RAM cost of a
// larger interaction table on ESP32-C3.
template <size_t MaxInteractions, size_t MaxHandlers>
class UiAppHost {
 public:
  using App = freeink::ui::FreeInkApp<MaxInteractions, MaxHandlers>;
  using Screen = typename App::ScreenType;

  explicit UiAppHost(const GfxRenderer& renderer)
      : target(makeUiTarget(renderer)), app(target, target.deviceContext()) {}

  void reset() {
    ready.store(false, std::memory_order_release);
    applySharedUiTheme(app, target);
  }

  void render() {
    app.setDevice(target.deviceContext());
    app.render();
    ready.store(true, std::memory_order_release);
  }

  bool routeTouch(const MappedInputManager& input, freeink::ui::ActionEvent& event) {
    if (!ready.load(std::memory_order_acquire)) return false;
    const auto snapshot = touchSnapshotFrom(input);
    if (!snapshot.touchPressed && !snapshot.touchReleased && !snapshot.touchHeld) return false;
    event = app.route(snapshot);
    return true;
  }

  void closeRouting() { ready.store(false, std::memory_order_release); }
  bool routingReady() const { return ready.load(std::memory_order_acquire); }

  // The target is declared before the app because FreeInkApp keeps a reference
  // to it. Both are allocation-free and live for the owning Activity lifetime.
  freeink::ui::GfxRendererTarget target;
  App app;

 private:
  std::atomic<bool> ready{false};
};
