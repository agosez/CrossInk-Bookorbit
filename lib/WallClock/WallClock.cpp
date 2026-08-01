#include "WallClock.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <ctime>

#ifndef SIMULATOR
#include <sys/time.h>
#endif

namespace {
constexpr char STATE_PATH[] = "/.crosspoint/wallclock.bin";
// Bump this (and document the layout in docs/file-formats.md) if the record layout
// changes: a file read under the wrong layout would mis-correct queued stats events,
// which store a truncated era per record. An unknown magic is discarded, which restarts
// era numbering — harmless as long as pending queues are drained first.
constexpr char STATE_MAGIC[4] = {'W', 'C', 'K', '1'};

// One NTP-measured era correction. Only eras where NTP actually ran appear in the
// history ring: an era that never saw NTP has no measurable error.
//
// `delta` is the error measured at `syncDeviceEpoch` (the clock reading just before the
// NTP sync). `windowStartEpoch` is the real time of the PREVIOUS sync in the same era,
// when there was one: that sync set the clock exactly, so the error was zero at that
// instant and grew to `delta` by `syncDeviceEpoch`. Those two points bound a drift ramp.
// It stays 0 when the era opened with a clock loss, because then the error starts at the
// unknown powered-off duration rather than at zero, and the ramp does not apply (see
// correctionForEvent).
struct EraCorrection {
  uint32_t era = 0;
  int64_t delta = 0;
  uint32_t windowStartEpoch = 0;
  uint32_t syncDeviceEpoch = 0;
  bool used = false;
};

// RAM mirror of the persisted state. Power eras are keyed to CLOCK CONTINUITY, not to
// RAM retention: the ESP32-C3's RTC memory does not reliably survive deep sleep on this
// hardware (observed in the field: every wake bumped a RTC_NOINIT-based era), but the
// system clock itself is restored/kept sane by initAtBoot. If the clock is plausible at
// boot, timestamps continue the same timeline -> same era.
struct WallClockState {
  uint32_t era = 0;
  uint32_t checkpointEpoch = 0;
  EraCorrection history[WallClock::ERA_HISTORY];
};
WallClockState s_state;
bool s_loaded = false;

const EraCorrection* findCorrection(const WallClockState& state, const uint32_t era) {
  for (size_t i = 0; i < WallClock::ERA_HISTORY; i++) {
    if (state.history[i].used && state.history[i].era == era) return &state.history[i];
  }
  return nullptr;
}

void upsertIntoState(WallClockState& state, const EraCorrection& entry) {
  // Update the era's existing slot, else replace the oldest (lowest era) entry.
  size_t slot = 0;
  uint32_t lowestEra = UINT32_MAX;
  for (size_t i = 0; i < WallClock::ERA_HISTORY; i++) {
    if (state.history[i].used && state.history[i].era == entry.era) {
      slot = i;
      break;
    }
    const uint32_t slotEra = state.history[i].used ? state.history[i].era : 0;
    if (slotEra < lowestEra) {
      lowestEra = slotEra;
      slot = i;
    }
  }
  state.history[slot] = entry;
}

bool plausible(uint64_t epoch) {
  return epoch >= WallClock::MIN_PLAUSIBLE_EPOCH && epoch < WallClock::MAX_PLAUSIBLE_EPOCH;
}

bool loadState(WallClockState& out) {
  if (!Storage.exists(STATE_PATH)) return false;
  FsFile file;
  if (!Storage.openFileForRead("CLK", STATE_PATH, file)) return false;

  char magic[4];
  uint8_t pad[3];
  bool ok = file.read(magic, sizeof(magic)) == sizeof(magic) && memcmp(magic, STATE_MAGIC, sizeof(STATE_MAGIC)) == 0 &&
            file.read(&out.era, sizeof(out.era)) == sizeof(out.era) &&
            file.read(&out.checkpointEpoch, sizeof(out.checkpointEpoch)) == sizeof(out.checkpointEpoch);
  for (size_t i = 0; ok && i < WallClock::ERA_HISTORY; i++) {
    EraCorrection entry;
    uint8_t used = 0;
    ok = file.read(&entry.era, sizeof(entry.era)) == sizeof(entry.era) &&
         file.read(&used, sizeof(used)) == sizeof(used) && file.read(pad, sizeof(pad)) == sizeof(pad) &&
         file.read(&entry.delta, sizeof(entry.delta)) == sizeof(entry.delta) &&
         file.read(&entry.windowStartEpoch, sizeof(entry.windowStartEpoch)) == sizeof(entry.windowStartEpoch) &&
         file.read(&entry.syncDeviceEpoch, sizeof(entry.syncDeviceEpoch)) == sizeof(entry.syncDeviceEpoch);
    if (ok && used != 0) {
      entry.used = true;
      upsertIntoState(out, entry);
    }
  }
  file.close();
  return ok;
}

void saveState() {
  FsFile file;
  if (!Storage.openFileForWrite("CLK", STATE_PATH, file)) {
    LOG_ERR("CLK", "Failed to persist wall-clock state");
    return;
  }
  const uint8_t pad[3] = {};
  bool ok = file.write(STATE_MAGIC, sizeof(STATE_MAGIC)) == sizeof(STATE_MAGIC) &&
            file.write(&s_state.era, sizeof(s_state.era)) == sizeof(s_state.era) &&
            file.write(&s_state.checkpointEpoch, sizeof(s_state.checkpointEpoch)) == sizeof(s_state.checkpointEpoch);
  for (size_t i = 0; ok && i < WallClock::ERA_HISTORY; i++) {
    const EraCorrection& entry = s_state.history[i];
    const uint8_t used = entry.used ? 1 : 0;
    ok = file.write(&entry.era, sizeof(uint32_t)) == sizeof(uint32_t) &&
         file.write(&used, sizeof(used)) == sizeof(used) && file.write(pad, sizeof(pad)) == sizeof(pad) &&
         file.write(&entry.delta, sizeof(int64_t)) == sizeof(int64_t) &&
         file.write(&entry.windowStartEpoch, sizeof(uint32_t)) == sizeof(uint32_t) &&
         file.write(&entry.syncDeviceEpoch, sizeof(uint32_t)) == sizeof(uint32_t);
  }
  file.close();
  if (!ok) {
    LOG_ERR("CLK", "Failed to write wall-clock state");
  }
}
}  // namespace

void WallClock::initAtBoot() {
  WallClockState persisted;
  if (loadState(persisted)) {
    s_state = persisted;
  }
  s_loaded = true;

  const time_t now = time(nullptr);
  if (plausible(static_cast<uint64_t>(now))) {
    // The clock survived (soft reset, or a deep sleep that retained timekeeping): the
    // timestamp timeline is continuous, so the era — and the correction anchors
    // established in it — carry over.
    LOG_INF("CLK", "Warm boot: era %u (synced=%d)", (unsigned)s_state.era,
            findCorrection(s_state, s_state.era) != nullptr ? 1 : 0);
    return;
  }

  // The clock was lost: a genuinely new timeline. Timestamps taken from here on are
  // offset differently from real time than the previous era's, so corrections must not
  // cross this boundary. The ending era's correction (if NTP ever measured it) already
  // lives in the history ring for later resolution.
  s_state.era++;
  if (plausible(s_state.checkpointEpoch)) {
#ifndef SIMULATOR
    // Restore an approximate clock: correct except for the time spent powered off.
    // NTP replaces it with exact time on the next network operation.
    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(s_state.checkpointEpoch);
    settimeofday(&tv, nullptr);
#endif
    LOG_INF("CLK", "Cold boot: clock restored from checkpoint %u (era %u)", (unsigned)s_state.checkpointEpoch,
            (unsigned)s_state.era);
  } else {
    LOG_INF("CLK", "Cold boot: era %u, clock unset (no checkpoint)", (unsigned)s_state.era);
  }
  saveState();
}

void WallClock::checkpoint() {
  const time_t now = time(nullptr);
  if (!plausible(static_cast<uint64_t>(now))) {
    LOG_DBG("CLK", "Skipping checkpoint: clock implausible");
    return;
  }
  s_state.checkpointEpoch = static_cast<uint32_t>(now);
  saveState();
}

void WallClock::markNtpSynced(const uint32_t epochBeforeSync) {
  const time_t now = time(nullptr);
  if (!plausible(static_cast<uint64_t>(now))) {
    LOG_ERR("CLK", "NTP sync reported but clock still implausible");
    return;
  }
  // The error the clock had accumulated by `epochBeforeSync`, which this sync just wiped
  // out by setting the clock to real time.
  const int64_t delta = static_cast<int64_t>(now) - static_cast<int64_t>(epochBeforeSync);

  // A previous sync in this same era left the clock exact at its own instant, so the
  // error ramped from 0 there to `delta` here and same-era timestamps in between can be
  // interpolated. Without one, the era opened on a clock loss: the error starts at the
  // unknown powered-off duration, so `delta` applies as a flat offset instead.
  uint32_t windowStart = 0;
  if (const EraCorrection* previous = findCorrection(s_state, s_state.era)) {
    const int64_t previousSyncRealTime = static_cast<int64_t>(previous->syncDeviceEpoch) + previous->delta;
    if (previous->syncDeviceEpoch != 0 && plausible(static_cast<uint64_t>(previousSyncRealTime))) {
      windowStart = static_cast<uint32_t>(previousSyncRealTime);
    }
  }
  upsertIntoState(s_state, {s_state.era, delta, windowStart, epochBeforeSync, true});
  s_state.checkpointEpoch = static_cast<uint32_t>(now);
  if (windowStart != 0) {
    LOG_INF("CLK", "NTP confirmed (era %u, drift %+lld s over %u s since last sync)", (unsigned)s_state.era,
            (long long)delta, (unsigned)(epochBeforeSync - windowStart));
  } else {
    LOG_INF("CLK", "NTP confirmed (era %u, correction %+lld s, flat)", (unsigned)s_state.era, (long long)delta);
  }
  saveState();
}

uint32_t WallClock::era() { return s_state.era; }

bool WallClock::correctionForEvent(const uint16_t truncatedEra, const uint32_t recordedEpoch, int64_t& outDelta) {
  if (!s_loaded) return false;
  const EraCorrection* entry = nullptr;
  for (size_t i = 0; i < ERA_HISTORY; i++) {
    if (s_state.history[i].used && static_cast<uint16_t>(s_state.history[i].era) == truncatedEra) {
      entry = &s_state.history[i];
      break;
    }
  }
  if (entry == nullptr) return false;

  // Recorded after that sync set the clock to real time: already correct, bar the drift
  // accrued since, which the next sync will measure and this one cannot know.
  if (entry->syncDeviceEpoch != 0 && recordedEpoch > entry->syncDeviceEpoch) {
    outDelta = 0;
    return true;
  }
  // Era opened on a clock loss: the whole block shifts by the measured error, keeping
  // the intervals between events intact.
  if (entry->windowStartEpoch == 0 || entry->syncDeviceEpoch <= entry->windowStartEpoch) {
    outDelta = entry->delta;
    return true;
  }
  // Recorded before this drift window, so an earlier sync had already corrected it.
  if (recordedEpoch <= entry->windowStartEpoch) {
    outDelta = 0;
    return true;
  }
  // Inside the window: the clock drifts at a near-constant rate (the RC oscillator's ppm
  // error), so the error at `recordedEpoch` is the fraction of the window elapsed.
  const int64_t window = static_cast<int64_t>(entry->syncDeviceEpoch) - static_cast<int64_t>(entry->windowStartEpoch);
  const int64_t elapsed = static_cast<int64_t>(recordedEpoch) - static_cast<int64_t>(entry->windowStartEpoch);
  const int64_t halfWindow = entry->delta >= 0 ? window / 2 : -(window / 2);
  outDelta = (entry->delta * elapsed + halfWindow) / window;
  return true;
}

bool WallClock::now(uint32_t& outEpochSeconds) {
  const time_t now = time(nullptr);
  outEpochSeconds = now > 0 ? static_cast<uint32_t>(now) : 0;
  return plausible(static_cast<uint64_t>(now));
}
