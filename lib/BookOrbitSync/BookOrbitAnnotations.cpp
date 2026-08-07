#include "BookOrbitAnnotations.h"

#include <MD5Builder.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "DaysFromCivil.h"

bool bookOrbitParseDatetime(const char* datetime, uint32_t& outEpochSeconds) {
  if (!datetime) return false;

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  // Read back as UTC without asking what zone the server meant. The value only has to survive a
  // round trip: it is reformatted the same way before being hashed or sent, so whatever offset
  // the string carried is preserved exactly.
  if (sscanf(datetime, "%4d-%2d-%2d %2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return false;
  }
  if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
      second > 60) {
    return false;
  }

  const int32_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  if (days < 0) return false;
  outEpochSeconds = static_cast<uint32_t>(days) * 86400u + static_cast<uint32_t>(hour) * 3600u +
                    static_cast<uint32_t>(minute) * 60u + static_cast<uint32_t>(second);
  return true;
}

bool bookOrbitFormatDatetime(const uint32_t epochSeconds, char (&outDatetime)[20]) {
  const time_t at = static_cast<time_t>(epochSeconds);
  struct tm utc = {};
  gmtime_r(&at, &utc);
  return strftime(outDatetime, sizeof(outDatetime), "%Y-%m-%d %H:%M:%S", &utc) != 0;
}

bool bookOrbitAnnotationKey(const char* datetime, const char* pos0,
                            char (&outKey)[BookOrbitAnnotationKey::DIGEST_SIZE]) {
  if (!datetime || !*datetime || !pos0 || !*pos0) return false;

  // md5(datetime | pos0), the same construction the reference plugin uses. Fed in three
  // pieces so neither field has to be concatenated into a temporary first.
  MD5Builder md5;
  md5.begin();
  md5.add(datetime);
  md5.add("|");
  md5.add(pos0);
  md5.calculate();

  const String hex = md5.toString();
  if (hex.length() + 1 != BookOrbitAnnotationKey::DIGEST_SIZE) return false;
  memcpy(outKey, hex.c_str(), BookOrbitAnnotationKey::DIGEST_SIZE);
  return true;
}
