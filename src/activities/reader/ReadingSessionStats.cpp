#include "ReadingSessionStats.h"

#include "ReadingDailyMinutes.h"

void commitReadingSession(BookReadingStats& book, GlobalReadingStats& global, const uint32_t seconds,
                          const ReadingStatsDateTime* sessionStart) {
  // Page intervals longer than the idle threshold are rejected before they
  // reach the session total, so seconds is active reading time.
  if (seconds >= 60) {
    book.sessionCount++;
    global.totalSessions++;
  }
  if (seconds < 10) return;
  book.totalReadingSeconds += seconds;
  global.totalReadingSeconds += seconds;
  // Before the check below: this one also runs on boards without a clock chip.
  ReadingDailyMinutes::record(sessionStart, seconds);
  if (!sessionStart) return;
  book.recordReadingSpan(*sessionStart, seconds);
  global.recordReadingSpan(*sessionStart, seconds);
  if (seconds >= 120 && !book.startDateManual && !book.startDate.isValid()) {
    book.startDate = sessionStart->date;
  }
}

void importBookStatsEdits(BookReadingStats& live, const BookReadingStats& disk) {
  live.isCompleted = disk.isCompleted;
  live.startDateManual = disk.startDateManual;
  live.finishedDateManual = disk.finishedDateManual;
  live.startDate = disk.startDate;
  live.finishedDate = disk.finishedDate;
}

void refreshGlobalStatsAfterOverlay(GlobalReadingStats& live, const uint32_t resetRevisionAtOpen) {
  GlobalReadingStats disk = GlobalReadingStats::load();
  if (GlobalReadingStats::localResetRevision() != resetRevisionAtOpen) {
    live = disk;
  } else {
    live.completedBooks = disk.completedBooks;
  }
}
