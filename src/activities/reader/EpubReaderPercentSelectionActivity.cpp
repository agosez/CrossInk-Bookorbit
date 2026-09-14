#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "DeviceCapabilities.h"
#include "MappedInputManager.h"
#include "components/SliderValue.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_SLIDER = 1;
constexpr fui::ActionId ACTION_STEP = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr fui::ActionId ACTION_CONFIRM = 4;
// Fine/coarse step sizes for physical input and the four touch step controls.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

EpubReaderPercentSelectionActivity::EpubReaderPercentSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const int initialPercent)
    : Activity("EpubReaderPercentSelection", renderer, mappedInput),
      percent(initialPercent),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_SLIDER, &EpubReaderPercentSelectionActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &EpubReaderPercentSelectionActivity::onStepEvent, this);
  app.on(ACTION_CANCEL, &EpubReaderPercentSelectionActivity::onCancelEvent, this);
  app.on(ACTION_CONFIRM, &EpubReaderPercentSelectionActivity::onConfirmEvent, this);
  app.setScreen(&EpubReaderPercentSelectionActivity::percentScreen, this);
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Wrap using a 100-value ring (0% and 100% are the same wrap point), but keep 100 as the
  // natural landing value when reached without crossing the boundary (e.g. 90 + 10 = 100).
  const int raw = percent + delta;
  if (raw > 0 && raw % 100 == 0) {
    percent = 100;
  } else {
    percent = ((raw % 100) + 100) % 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::setPercent(const int value) {
  const int clamped = std::clamp(value, 0, 100);
  if (clamped == percent) return;
  percent = clamped;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(user);
  if (event.dragPermille < 0) return;
  const int value = (static_cast<int>(event.dragPermille) * 100 + 500) / 1000;
  self->setPercent(self->sliderTapPending ? snapSliderTapValue(value, 0, 100, 5) : value);
}

void EpubReaderPercentSelectionActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->adjustPercent(event.value);
}

void EpubReaderPercentSelectionActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(user);
  self->app.clearTapFlash();
  self->cancel();
}

void EpubReaderPercentSelectionActivity::onConfirmEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(user);
  self->app.clearTapFlash();
  self->confirm();
}

void EpubReaderPercentSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderPercentSelectionActivity::confirm() {
  setResult(PercentResult{percent});
  finish();
}

void EpubReaderPercentSelectionActivity::loop() {
  // Touch goes through the FreeInkApp: render() registered the slider, step controls,
  // and actions; the slider follows the finger via InputDrag (dragPermille per held frame).
  // Runs before the Back handler because the release of a drag can also register as a
  // swipe (e.g. the left-edge rightward back gesture), so the drag must consume it so it
  // can't cancel the dialog or step the percent.
  fui::InputSnapshot snap{};
  if (uiReady) {
    snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      sliderTapPending = snap.touchReleased && snap.touchX >= 0;
      const auto event = app.route(snap);
      sliderTapPending = false;
      if (app.invalidated()) requestUpdate();
      if (event) {
        if (event.dragPermille >= 0) draggingSlider = true;
        return;
      }
    }
    if (draggingSlider) {
      // Drag ended (possibly off the slider): swallow the tap/swipe events it produced.
      if (!snap.touchHeld) draggingSlider = false;
      return;
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    cancel();
    return;
  }

  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    adjustPercent(kLargeStep);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Left) {
    adjustPercent(-kLargeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  // On edge-button boards (X3, X4 Pro) the side buttons sit on the left/right edges of the screen rather
  // than as a vertical up/down rocker (X4), so BTN_UP is physically the left button and BTN_DOWN the right
  // one. Flip the large-step direction there so the left button decreases and the right button increases.
  const int upDelta = deviceHasEdgeSideButtons(gpio) ? -kLargeStep : kLargeStep;
  const int downDelta = deviceHasEdgeSideButtons(gpio) ? kLargeStep : -kLargeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustPercent(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustPercent(downDelta); });
}

void EpubReaderPercentSelectionActivity::percentScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->buildPercentScreen(screen);
}

void EpubReaderPercentSelectionActivity::buildPercentScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Start below CrossInk's shared back header; its touch-device height differs from
  // the legacy theme header height.
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                           metrics.verticalSpacing * 4),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  char line[64];

  // Percent readout, centered above the slider.
  fui::TextStyle readout = theme.titleText;
  readout.align = fui::TextAlign::Center;
  const int16_t readoutLh = screen.target().lineHeight(readout.font);
  snprintf(line, sizeof(line), "%d%%", percent);
  screen.target().text(screen.takeTop(readoutLh, theme.spaceLg), line, readout);

  // The slider owns the full row. FreeInkUI expands the hit rect to the theme's
  // minimum touch size and keeps a drag captured after the finger leaves the track.
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  const fui::Rect row = screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset);

  fui::SliderProps props;
  props.value = percent;
  props.max = 100;
  props.action = ACTION_SLIDER;
  props.inputMask = fui::InputTouch | fui::InputDrag;
  fui::slider(screen.frame(), row, props);

  if (mappedInput.hasTouch()) {
    // Preserve CrossInk's four direct coarse/fine step controls, but let the
    // FreeInkApp own both their rendering and hit testing.
    constexpr int16_t deltas[] = {-kLargeStep, -kSmallStep, kSmallStep, kLargeStep};
    constexpr const char* labels[] = {"-10", "-1", "+1", "+10"};
    const fui::Rect stepBand = screen.takeTop(theme.rowHeight, theme.spaceLg);
    const int16_t gap = theme.spaceMd;
    const int16_t width = static_cast<int16_t>((stepBand.width - gap * 3) / 4);
    for (int index = 0; index < 4; ++index) {
      fui::ButtonProps button;
      button.label = labels[index];
      button.action = ACTION_STEP;
      button.value = deltas[index];
      button.inputMask = fui::InputTouch;
      screen.button(button, fui::Rect{static_cast<int16_t>(stepBand.x + index * (width + gap)), stepBand.y, width,
                                      stepBand.height});
    }

    const int16_t actionGap = theme.spaceMd;
    // Reserve at least as much vertical space as each button needs. Otherwise
    // FreeInkUI expands the smaller rows to minTouchSize after the bottom band
    // has been placed, which can draw the Cancel action beyond the screen.
    const int16_t actionHeight = std::max<int16_t>(theme.rowHeight, 56);
    const int16_t actionBandHeight = static_cast<int16_t>(actionHeight * 2 + actionGap);
    const fui::Rect actionBand = screen.takeBottom(actionBandHeight, theme.spaceLg);
    // Keep these explicit full-width actions aligned with the inset used by
    // every TouchActionButtons group, rather than drawing against the bezel.
    const int16_t actionSideInset = static_cast<int16_t>(metrics.contentSidePadding);
    const fui::Rect actionArea{static_cast<int16_t>(actionBand.x + actionSideInset), actionBand.y,
                               std::max<int16_t>(1, static_cast<int16_t>(actionBand.width - actionSideInset * 2)),
                               actionBand.height};
    fui::ButtonProps action;
    action.inputMask = fui::InputTouch;
    action.styles = fui::outlinedButtonStyles();
    action.text = theme.bodyText;
    action.text.align = fui::TextAlign::Center;
    action.minTouchSize = actionHeight;
    action.label = tr(STR_CONFIRM);
    action.action = ACTION_CONFIRM;
    action.text.bold = true;
    screen.button(action, fui::Rect{actionArea.x, actionArea.y, actionArea.width, actionHeight});
    action.label = tr(STR_CANCEL);
    action.action = ACTION_CANCEL;
    action.text.bold = false;
    screen.button(action, fui::Rect{actionArea.x, static_cast<int16_t>(actionArea.y + actionHeight + actionGap),
                                    actionArea.width, actionHeight});
    return;
  }

  // Two-line step hint built from separate label + value strings (front buttons = fine step, side
  // buttons = coarse step), so the layout doesn't depend on a separator hidden in translated text.
  fui::TextStyle hint = theme.smallText;
  hint.align = fui::TextAlign::Center;
  const int16_t hintLh = screen.target().lineHeight(hint.font);
  snprintf(line, sizeof(line), "%s %d%%", I18N.get(StrId::STR_STEP_HINT_FRONT), kSmallStep);
  screen.target().text(screen.takeTop(hintLh, theme.spaceSm), line, hint);
  snprintf(line, sizeof(line), "%s %d%%", I18N.get(StrId::STR_STEP_HINT_SIDE), kLargeStep);
  screen.target().text(screen.takeTop(hintLh), line, hint);
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_GO_TO_PERCENT), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_GO_TO_PERCENT), nullptr, true);
  }

  // Percent readout, slider, step controls, and actions render through the app.
  uiReady = false;
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;

  // Button hints follow the current front button layout and auto-hide on touch devices.
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
