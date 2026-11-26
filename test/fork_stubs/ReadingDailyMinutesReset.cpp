// GlobalReadingStats::resetLocal() also clears this fork's per-day reading store
// (ReadingDailyMinutes.cpp), whose file I/O the upstream stats test stubs cannot build.
// Its own suite is test/reading_daily_minutes; here the reset just succeeds.
#include "ReadingDailyMinutes.h"

bool ReadingDailyMinutes::reset() { return true; }
