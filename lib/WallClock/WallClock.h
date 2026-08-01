#pragma once
#include <cstddef>
#include <cstdint>

/**
 * Best-effort wall-clock keeper for devices without a battery-backed RTC (X4).
 *
 * The ESP32-C3 system clock runs off the RTC timer, so it survives deep sleep and
 * software resets; it is only lost on a true cold boot (power loss, brownout,
 * flashing). This module layers two mechanisms on top:
 *
 *  - Checkpointing: whenever the clock is trustworthy (after NTP, at reading-session
 *    ends), the current epoch is persisted to SD. On a cold boot the clock is
 *    restored from that checkpoint, so it is only off by the time spent powered
 *    down instead of starting at 1970.
 *
 *  - Power eras: a counter that increments whenever the clock is lost, so a queued
 *    timestamp records which timeline it belongs to. Consumers that queue timestamped
 *    records (e.g. BookOrbitStatsQueue) retroactively correct them to real time once
 *    NTP runs, via correctionForEvent(). Within one era the clock only ever drifts
 *    (any clock loss opens a new era), so the correction is either a drift ramp or a
 *    flat shift — see correctionForEvent().
 *
 * On X3 hardware the DS3231 RTC remains the primary time source for reading-stats
 * dates; this module still keeps the system clock sane for everything else.
 */
class WallClock {
 public:
  // Call once at boot after storage is ready. Detects cold boots (advances the era
  // and, when the system clock is implausible, restores it from the checkpoint).
  static void initAtBoot();

  // Persist the current time as the boot-restore checkpoint. No-op (with a debug
  // log) when the current time is implausible. Cheap: one small SD file write.
  static void checkpoint();

  // Record that NTP just set the system clock. `epochBeforeSync` is time(nullptr)
  // captured immediately before the NTP sync; the difference to the corrected clock is
  // the error the clock had accumulated, and the pair (epochBeforeSync, real time)
  // anchors this era's correction. Also checkpoints.
  static void markNtpSynced(uint32_t epochBeforeSync);

  // Cold-boot counter. Wraps into uint16_t storage in queue records; wrap is
  // harmless (eras only need to be distinguishable from their neighbours).
  static uint32_t era();

  // Number of past eras whose NTP-measured corrections are remembered (ring buffer
  // in the persisted state). Eras that never saw NTP have no measurable correction
  // and are never in the table.
  static constexpr size_t ERA_HISTORY = 6;

  // Seconds to add to a timestamp recorded in `truncatedEra` (as stored truncated in
  // queue records) to get real time. Covers the current era and up to ERA_HISTORY
  // confirmed past eras, so timestamps survive several clock losses between syncs.
  // Returns false when that era's error was never measured.
  //
  // The clock's error has two sources, and the era tells them apart:
  //
  //  - Drift, always present: the RTC timer runs on the internal RC oscillator
  //    (~3500 ppm, minutes per day). It accumulates in proportion to elapsed time, so
  //    between two syncs in the SAME era the error ramps from 0 to the newly measured
  //    value and is interpolated for each timestamp in between.
  //  - Powered-off time, lost at each clock loss: the restored checkpoint is behind
  //    real time by however long the device was off. That is a step, not a ramp, and it
  //    is unknowable until the next sync — but it also opens a new era, so it never
  //    lands inside a drift window. Those eras take the measured error as a FLAT shift,
  //    which keeps the intervals between their events intact.
  //
  // Interpolating across a step would smear it over events on both sides, which is why
  // this takes the timestamp and not just the era.
  static bool correctionForEvent(uint16_t truncatedEra, uint32_t recordedEpoch, int64_t& outDelta);

  // Current UTC epoch seconds, always returned (may be checkpoint-approximate or
  // even 1970-based right after a first-ever cold boot). Returns false when the
  // value is implausible (< 2020), true when plausible.
  static bool now(uint32_t& outEpochSeconds);

  static constexpr uint32_t MIN_PLAUSIBLE_EPOCH = 1577836800u;  // 2020-01-01
  static constexpr uint32_t MAX_PLAUSIBLE_EPOCH = 4102444800u;  // 2100-01-01
};
