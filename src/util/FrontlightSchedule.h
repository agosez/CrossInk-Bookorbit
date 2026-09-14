#pragma once

#include <cstdint>

// Daily frontlight schedules are stored as local minutes since midnight.
// Keeping the policy free of hardware dependencies lets boot code and host
// tests use the same boundary rules.
namespace FrontlightSchedule {
constexpr uint16_t kMinutesPerDay = 24 * 60;
constexpr uint16_t kUnsetTimeOfDay = 0xFFFF;

struct TimeOfDay {
  uint8_t hour12;
  uint8_t minute;
  bool isPm;
};

constexpr bool isTimeOfDayValid(const uint16_t timeOfDay) { return timeOfDay < kMinutesPerDay; }

constexpr bool hasCompleteWindow(const bool enabled, const uint16_t startTimeOfDay, const uint16_t endTimeOfDay) {
  return enabled && isTimeOfDayValid(startTimeOfDay) && isTimeOfDayValid(endTimeOfDay) &&
         startTimeOfDay != endTimeOfDay;
}

// Network transitions are silent restarts for memory recovery, but their
// frontlight should still follow the user's wake preference.
constexpr bool shouldPreserveLightAcrossRestart(const bool isSilentReboot, const bool followsWakeLightPolicy) {
  return isSilentReboot && !followsWakeLightPolicy;
}

constexpr bool shouldRestoreLightOnStart(const bool preserveLightAcrossRestart, const bool restoreOnWake,
                                         const bool wasLightOnBeforeSleep) {
  return wasLightOnBeforeSleep && (preserveLightAcrossRestart || restoreOnWake);
}

constexpr bool shouldApplyOnWakeSchedule(const bool preserveLightAcrossRestart, const bool restoreOnWake,
                                         const bool wasLightOnBeforeSleep) {
  return !preserveLightAcrossRestart && (!restoreOnWake || !wasLightOnBeforeSleep);
}

constexpr TimeOfDay timeOfDayFromMinutes(const uint16_t timeOfDay) {
  // New endpoints begin at noon so the AM/PM picker has a useful, explicit
  // default instead of presenting the storage-oriented 00:00 value.
  if (!isTimeOfDayValid(timeOfDay)) return {12, 0, true};

  const uint8_t hour24 = static_cast<uint8_t>(timeOfDay / 60);
  const uint8_t hour12 = hour24 % 12 == 0 ? 12 : hour24 % 12;
  return {hour12, static_cast<uint8_t>(timeOfDay % 60), hour24 >= 12};
}

constexpr uint16_t minutesFromTimeOfDay(const uint8_t hour12, const uint8_t minute, const bool isPm) {
  const uint8_t normalizedHour = hour12 >= 1 && hour12 <= 12 ? hour12 : 12;
  const uint8_t normalizedMinute = minute < 60 ? minute : 0;
  uint8_t hour24 = normalizedHour % 12;
  if (isPm) hour24 = static_cast<uint8_t>(hour24 + 12);
  return static_cast<uint16_t>(hour24 * 60 + normalizedMinute);
}

constexpr bool containsTimeOfDay(const uint16_t startTimeOfDay, const uint16_t endTimeOfDay,
                                 const uint16_t currentTimeOfDay) {
  if (!isTimeOfDayValid(startTimeOfDay) || !isTimeOfDayValid(endTimeOfDay) || !isTimeOfDayValid(currentTimeOfDay) ||
      startTimeOfDay == endTimeOfDay) {
    return false;
  }
  return startTimeOfDay < endTimeOfDay ? currentTimeOfDay >= startTimeOfDay && currentTimeOfDay < endTimeOfDay
                                       : currentTimeOfDay >= startTimeOfDay || currentTimeOfDay < endTimeOfDay;
}

constexpr uint16_t localTimeOfDay(const uint8_t utcHour, const uint8_t utcMinute,
                                  const uint8_t utcOffsetQuarterHoursBiased) {
  const int offsetQuarterHours =
      static_cast<int>(utcOffsetQuarterHoursBiased > 104 ? 104 : utcOffsetQuarterHoursBiased) - 48;
  const int utcMinutes = static_cast<int>(utcHour) * 60 + static_cast<int>(utcMinute);
  const int localMinutes = ((utcMinutes + offsetQuarterHours * 15) % 1440 + 1440) % 1440;
  return static_cast<uint16_t>(localMinutes);
}

}  // namespace FrontlightSchedule
