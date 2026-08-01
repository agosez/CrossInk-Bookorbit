#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "BookOrbitSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with a BookOrbit server.
 *
 * Mirrors KOReaderSyncActivity's flow exactly (see that file for the detailed
 * rationale); the only protocol difference is the BookOrbit-specific client/store
 * and that BookOrbit always identifies documents by the binary partial-MD5 hash.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class BookOrbitSyncActivity final : public Activity {
 public:
  explicit BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                 int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                 KOReaderPosition localKoPos, std::string localChapterName,
                                 std::optional<uint16_t> currentParagraphIndex = std::nullopt)
      : Activity("BookOrbitSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader-format wire data (pre-computed before Epub was released)
  KOReaderPosition localProgress;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // See KOReaderSyncActivity for why this tracking exists (esp_wifi_stop() during
  // performUpload() makes WiFi.getMode() unreliable for the onExit() decision).
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  void uploadQueuedStats();
  bool consumeInitialConfirmRelease();
  void ensureEpubLoaded();
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToReader();
};
