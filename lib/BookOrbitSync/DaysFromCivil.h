#pragma once
#include <cstdint>

// Howard Hinnant's days_from_civil: days since 1970-01-01 for a Gregorian date. Both the
// stats queue (RTC readout) and the annotation datetime helpers need this conversion, and
// timegm() is not dependable on this platform.
inline int32_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(year - era * 400);
  const uint32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}
