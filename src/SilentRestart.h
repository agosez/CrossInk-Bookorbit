#pragma once

#include <cstdint>
#include <string>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

enum class NetworkBootTarget : uint32_t {
  OTA = 2,
  OPDS = 3,
  KOREADER_SYNC = 4,
  KOREADER_AUTH = 5,
  FILE_TRANSFER = 6,
  MANAGE_FONTS = 7,
  BOOKORBIT_SYNC = 8,
  // The BookOrbit sync that runs behind the sleep screen before deep sleep (see
  // enterDeepSleep()). Its payload is BOOKORBIT_SLEEP_SYNC_PAYLOAD_POWER_HELD.
  BOOKORBIT_SLEEP_SYNC = 9,
};

// BOOKORBIT_SYNC payload: bit 16 set means bits 0-15 carry the paragraph index of the
// position being synced. The rest of the position survives the reboot as saved progress
// on the SD card; the paragraph anchor exists only in reader RAM, so it rides here.
constexpr uint32_t BOOKORBIT_SYNC_PAYLOAD_HAS_PARAGRAPH = 1u << 16;
// Bits 17-19 carry the reader's on-screen orientation plus one (0 = no reader
// override). A per-book orientation lives in SETTINGS only while the reader is open
// and its teardown restores the global value, so without this the sync screens would
// come back from the minimal network reboot in the global orientation.
// (Mirrors the KOReader sync orientation payload.)
constexpr uint32_t BOOKORBIT_SYNC_PAYLOAD_ORIENTATION_SHIFT = 17u;
constexpr uint32_t BOOKORBIT_SYNC_PAYLOAD_ORIENTATION_MASK = 0x7u << BOOKORBIT_SYNC_PAYLOAD_ORIENTATION_SHIFT;
// BOOKORBIT_SLEEP_SYNC payload: Power was still held when the device went to sleep (a
// long-press sleep), so that press belongs to the sleep gesture and must be released
// before a press can wake the device out of the sync.
constexpr uint32_t BOOKORBIT_SLEEP_SYNC_PAYLOAD_POWER_HELD = 1u << 0;

constexpr bool isNetworkBootTargetValue(const uint32_t value) {
  switch (static_cast<NetworkBootTarget>(value)) {
    case NetworkBootTarget::OTA:
    case NetworkBootTarget::OPDS:
    case NetworkBootTarget::KOREADER_SYNC:
    case NetworkBootTarget::KOREADER_AUTH:
    case NetworkBootTarget::FILE_TRANSFER:
    case NetworkBootTarget::MANAGE_FONTS:
    case NetworkBootTarget::BOOKORBIT_SYNC:
    case NetworkBootTarget::BOOKORBIT_SLEEP_SYNC:
      return true;
  }
  return false;
}

static_assert(isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::OTA)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::OPDS)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::KOREADER_SYNC)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::KOREADER_AUTH)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::FILE_TRANSFER)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::MANAGE_FONTS)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::BOOKORBIT_SYNC)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::BOOKORBIT_SLEEP_SYNC)),
              "Every network boot target must pass RTC target validation");

void silentRestart();                                            // home screen
void silentRestartToReader(bool cleanImageBaseOnEntry = false);  // currently-open EPUB (APP_STATE.openEpubPath)
// Reboots immediately after an activity releases exclusive raw storage.
void restartToHomeAfterStorageHandoff();
void silentRestartToNetwork(NetworkBootTarget target, uint32_t payload = 0);
void silentRestartToManageFonts();

// The sleep sync (NetworkBootTarget::BOOKORBIT_SLEEP_SYNC) runs while the device looks
// asleep, so Power is its only input: the press that would wake the device.
bool sleepSyncWakeRequested();
// Abandons the sleep sync for that press and boots the way a power-button wake would.
void wakeFromSleepSync();
// Ends the sleep sync by entering the deep sleep it postponed (or waking, if Power
// was pressed meanwhile). The sleep screen is still on the panel and is left as is.
void completeSleepAfterSleepSync();

void armSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t spineIndex, uint16_t targetPage,
                                     bool autoPageTurnActive);
bool consumeSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t& spineIndex, uint16_t& targetPage,
                                         bool& autoPageTurnActive);
