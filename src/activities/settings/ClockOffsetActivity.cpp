#include "ClockOffsetActivity.h"

#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"

namespace {
constexpr uint8_t MAX_POS_HOURS = 14;
constexpr uint8_t MAX_NEG_HOURS = 12;
constexpr uint8_t MINUTE_STEPS = 4;  // 0, 15, 30, 45
constexpr uint8_t MINUTES_PER_QUARTER = 15;
constexpr uint8_t BIAS_QUARTER_HOURS = 48;  // 0 stored = UTC-12, 48 stored = UTC+0
constexpr int TOUCH_BUTTON_SIZE = 60;
constexpr int TOUCH_BUTTON_STACK_GAP = 12;
constexpr int TOUCH_BUTTON_GAP = 24;
constexpr int FIELD_HEIGHT = 60;
constexpr int SIGN_FIELD_MIN_WIDTH = 60;
constexpr int VALUE_FIELD_MIN_WIDTH = 72;
constexpr int LABEL_GAP = 18;
constexpr int FIELD_GAP = 14;
constexpr int COLON_GAP = 8;

struct PickerLayout {
  Rect signRect;
  Rect hoursRect;
  Rect minutesRect;
  Rect upRect;
  Rect downRect;
  int labelX;
  int colonX;
  int textY;
};

PickerLayout getPickerLayout(const GfxRenderer& renderer, const bool showTouchControls) {
  auto widthOf = [&](const char* text) { return renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD); };

  const int labelWidth = widthOf("UTC");
  const int signWidth = std::max(SIGN_FIELD_MIN_WIDTH, std::max(widthOf("+"), widthOf("-")) + 24);
  const int hoursWidth = std::max(VALUE_FIELD_MIN_WIDTH, std::max(widthOf("14"), widthOf("12")) + 24);
  const int colonWidth = widthOf(":");
  const int minutesWidth =
      std::max(VALUE_FIELD_MIN_WIDTH, std::max({widthOf("00"), widthOf("15"), widthOf("30"), widthOf("45")}) + 24);
  const int pickerWidth =
      labelWidth + LABEL_GAP + signWidth + FIELD_GAP + hoursWidth + COLON_GAP + colonWidth + COLON_GAP + minutesWidth;
  const int arrowsWidth = showTouchControls ? TOUCH_BUTTON_GAP + TOUCH_BUTTON_SIZE : 0;
  const int contentWidth = pickerWidth + arrowsWidth;
  const int contentX = (renderer.getScreenWidth() - contentWidth) / 2;
  const int stackHeight = TOUCH_BUTTON_SIZE * 2 + TOUCH_BUTTON_STACK_GAP;
  const int stackY = renderer.getScreenHeight() / 2 - 25 - stackHeight / 2;
  const int fieldY = stackY + (stackHeight - FIELD_HEIGHT) / 2;
  const int textY = fieldY + (FIELD_HEIGHT - renderer.getLineHeight(UI_12_FONT_ID)) / 2;

  int x = contentX;
  const int labelX = x;
  x += labelWidth + LABEL_GAP;
  const Rect signRect{x, fieldY, signWidth, FIELD_HEIGHT};
  x += signWidth + FIELD_GAP;
  const Rect hoursRect{x, fieldY, hoursWidth, FIELD_HEIGHT};
  x += hoursWidth + COLON_GAP;
  const int colonX = x;
  x += colonWidth + COLON_GAP;
  const Rect minutesRect{x, fieldY, minutesWidth, FIELD_HEIGHT};
  x += minutesWidth;

  const int buttonX = x + TOUCH_BUTTON_GAP;
  const Rect upRect{buttonX, stackY, TOUCH_BUTTON_SIZE, TOUCH_BUTTON_SIZE};
  const Rect downRect{buttonX, stackY + TOUCH_BUTTON_SIZE + TOUCH_BUTTON_STACK_GAP, TOUCH_BUTTON_SIZE,
                      TOUCH_BUTTON_SIZE};
  return PickerLayout{signRect, hoursRect, minutesRect, upRect, downRect, labelX, colonX, textY};
}

// Convert a (sign, hours, quarter) triple into the biased storage value.
// Returns a value in [0, 104].
uint8_t encodeOffset(uint8_t sign, uint8_t hours, uint8_t quarter) {
  int signedQuarter = static_cast<int>(hours) * 4 + static_cast<int>(quarter);
  if (sign == 1) signedQuarter = -signedQuarter;
  int biased = signedQuarter + BIAS_QUARTER_HOURS;
  if (biased < 0) biased = 0;
  if (biased > 104) biased = 104;
  return static_cast<uint8_t>(biased);
}

// Decompose the biased storage value into (sign, hours, quarter).
void decodeOffset(uint8_t biased, uint8_t& sign, uint8_t& hours, uint8_t& quarter) {
  if (biased > 104) biased = BIAS_QUARTER_HOURS;
  int signedQuarter = static_cast<int>(biased) - BIAS_QUARTER_HOURS;
  if (signedQuarter < 0) {
    sign = 1;
    signedQuarter = -signedQuarter;
  } else {
    sign = 0;
  }
  hours = static_cast<uint8_t>(signedQuarter / 4);
  quarter = static_cast<uint8_t>(signedQuarter % 4);
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}
}  // namespace

void ClockOffsetActivity::onEnter() {
  Activity::onEnter();
  loadFromSettings();
  activeField = FIELD_SIGN;
  requestUpdate();
}

void ClockOffsetActivity::onExit() {
  saveToSettings();
  Activity::onExit();
}

void ClockOffsetActivity::loadFromSettings() {
  decodeOffset(SETTINGS.clockUtcOffsetQ, sign, hours, minutesQuarter);
  clampForSign();
}

void ClockOffsetActivity::saveToSettings() const {
  const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
  if (encoded == SETTINGS.clockUtcOffsetQ) return;
  SETTINGS.clockUtcOffsetQ = encoded;
  SETTINGS.saveToFile();
}

void ClockOffsetActivity::clampForSign() {
  const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
  if (hours > maxHours) hours = maxHours;
  // At the absolute boundary (-12:00 or +14:00) only :00 is valid.
  if (hours == maxHours && minutesQuarter != 0) {
    minutesQuarter = 0;
  }
}

void ClockOffsetActivity::adjustActiveField(int delta) {
  switch (activeField) {
    case FIELD_SIGN: {
      sign = static_cast<uint8_t>((sign + 1) % 2);
      clampForSign();
      break;
    }
    case FIELD_HOURS: {
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      const int next = (static_cast<int>(hours) + delta + (maxHours + 1)) % (maxHours + 1);
      hours = static_cast<uint8_t>(next);
      clampForSign();
      break;
    }
    case FIELD_MINUTES: {
      // At the boundary hour, lock minutes to :00.
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      if (hours == maxHours) {
        minutesQuarter = 0;
        break;
      }
      const int next = (static_cast<int>(minutesQuarter) + delta + MINUTE_STEPS) % MINUTE_STEPS;
      minutesQuarter = static_cast<uint8_t>(next);
      break;
    }
    default:
      break;
  }
}

bool ClockOffsetActivity::fieldFromPoint(const int x, const int y, Field& field) const {
  const PickerLayout layout = getPickerLayout(renderer, mappedInput.hasTouch());
  if (contains(layout.signRect, x, y)) {
    field = FIELD_SIGN;
    return true;
  }
  if (contains(layout.hoursRect, x, y)) {
    field = FIELD_HOURS;
    return true;
  }
  if (contains(layout.minutesRect, x, y)) {
    field = FIELD_MINUTES;
    return true;
  }
  return false;
}

void ClockOffsetActivity::getTouchControlRects(Rect& upRect, Rect& downRect) const {
  const PickerLayout layout = getPickerLayout(renderer, true);
  upRect = layout.upRect;
  downRect = layout.downRect;
}

void ClockOffsetActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activeField = static_cast<Field>((activeField + 1) % FIELD_COUNT);
    requestUpdate();
    return;
  }

  if (mappedInput.hasTouch()) {
    int tx = 0;
    int ty = 0;
    Rect upRect;
    Rect downRect;
    getTouchControlRects(upRect, downRect);

    if (mappedInput.wasScreenTouchDown(tx, ty)) {
      if (contains(downRect, tx, ty) || contains(upRect, tx, ty)) {
        return;
      }
      Field touchedField = FIELD_HOURS;
      if (fieldFromPoint(tx, ty, touchedField)) {
        if (activeField != touchedField) {
          activeField = touchedField;
          requestUpdate();
        }
        return;
      }
    }

    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (contains(downRect, tx, ty)) {
        adjustActiveField(-1);
        requestUpdate();
        return;
      }
      if (contains(upRect, tx, ty)) {
        adjustActiveField(+1);
        requestUpdate();
        return;
      }

      Field touchedField = FIELD_HOURS;
      if (fieldFromPoint(tx, ty, touchedField)) {
        activeField = touchedField;
        requestUpdate();
        return;
      }
    }
  }

  buttonNavigator.onNextRelease([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
}

void ClockOffsetActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_CLOCK_UTC_OFFSET), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_CLOCK_UTC_OFFSET));
  }

  const PickerLayout layout = getPickerLayout(renderer, mappedInput.hasTouch());
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD); };

  char signStr[2] = {sign == 1 ? '-' : '+', '\0'};
  char hoursStr[8];
  snprintf(hoursStr, sizeof(hoursStr), "%d", hours);
  char minutesStr[8];
  snprintf(minutesStr, sizeof(minutesStr), "%02d", minutesQuarter * MINUTES_PER_QUARTER);

  renderer.drawText(UI_12_FONT_ID, layout.labelX, layout.textY, "UTC", true, EpdFontFamily::BOLD);

  auto drawField = [&](const char* text, const Rect& rect, const Field field) {
    const bool selected = activeField == field;
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, selected ? Color::LightGray : Color::White);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
    if (selected) {
      renderer.drawRect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, true);
    }
    const int textX = rect.x + (rect.width - widthOf(text)) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, layout.textY, text, true, EpdFontFamily::BOLD);
  };

  drawField(signStr, layout.signRect, FIELD_SIGN);
  drawField(hoursStr, layout.hoursRect, FIELD_HOURS);
  renderer.drawText(UI_12_FONT_ID, layout.colonX, layout.textY, ":", true, EpdFontFamily::BOLD);
  drawField(minutesStr, layout.minutesRect, FIELD_MINUTES);

  if (mappedInput.hasTouch()) {
    Rect upRect;
    Rect downRect;
    getTouchControlRects(upRect, downRect);
    auto drawTouchButton = [&](const Rect& rect, const freeink::Icon& icon) {
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::White);
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
      const freeink::ui::BitmapRef bitmap{icon.bits, icon.w, icon.h, freeink::ui::BitmapFormat::Mask1, true};
      freeink::ui::forEachBitmapPixel(
          freeink::ui::Rect{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y),
                            static_cast<int16_t>(rect.width), static_cast<int16_t>(rect.height)},
          bitmap, freeink::ui::BitmapMode::Center,
          [this](const int16_t px, const int16_t py) { renderer.drawPixel(px, py, true); });
    };
    drawTouchButton(upRect, icon_chevron_up_32);
    drawTouchButton(downRect, icon_chevron_down_32);
  }

  // Live preview of the resulting wall-clock time, so users can verify against a watch.
  if (halClock.isAvailable()) {
    char timeBuf[9];
    const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), encoded, SETTINGS.clockFormat == 1)) {
      // STR_CURRENT_TIME alone is 26 bytes in Russian and 24 in Arabic and
      // Ukrainian, before the separator and formatted time are appended.
      char preview[64];
      snprintf(preview, sizeof(preview), "%s %s", tr(STR_CURRENT_TIME), timeBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, layout.downRect.y + layout.downRect.height + 24, preview);
    }
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_NEXT_FIELD), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
