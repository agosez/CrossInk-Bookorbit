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
};

// BOOKORBIT_SYNC payload: bit 16 set means bits 0-15 carry the paragraph index of the
// position being synced. The rest of the position survives the reboot as saved progress
// on the SD card; the paragraph anchor exists only in reader RAM, so it rides here.
constexpr uint32_t BOOKORBIT_SYNC_PAYLOAD_HAS_PARAGRAPH = 1u << 16;

constexpr bool isNetworkBootTargetValue(const uint32_t value) {
  switch (static_cast<NetworkBootTarget>(value)) {
    case NetworkBootTarget::OTA:
    case NetworkBootTarget::OPDS:
    case NetworkBootTarget::KOREADER_SYNC:
    case NetworkBootTarget::KOREADER_AUTH:
    case NetworkBootTarget::FILE_TRANSFER:
    case NetworkBootTarget::MANAGE_FONTS:
    case NetworkBootTarget::BOOKORBIT_SYNC:
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
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::BOOKORBIT_SYNC)),
              "Every network boot target must pass RTC target validation");

void silentRestart();                                            // home screen
void silentRestartToReader(bool cleanImageBaseOnEntry = false);  // currently-open EPUB (APP_STATE.openEpubPath)
// Reboots immediately after an activity releases exclusive raw storage.
void restartToHomeAfterStorageHandoff();
void silentRestartToNetwork(NetworkBootTarget target, uint32_t payload = 0);
void silentRestartToManageFonts();

void armSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t spineIndex, uint16_t targetPage,
                                     bool autoPageTurnActive);
bool consumeSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t& spineIndex, uint16_t& targetPage,
                                         bool& autoPageTurnActive);
