#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity {
 public:
  static constexpr const char* NAME = "KOReaderSync";

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t readerOrientation)
      : Activity(NAME, renderer, mappedInput),
        currentSpineIndex(0),
        currentPage(0),
        totalPagesInSpine(1),
        primaryMatchMethod(DocumentMatchMethod::FILENAME),
        remoteMatchMethod(DocumentMatchMethod::FILENAME),
        remoteProgress{},
        remotePosition{},
        localProgress{},
        restartBeforeNetwork(true),
        readerOrientation(readerOrientation) {}

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                DocumentMatchMethod matchMethod, const uint8_t readerOrientation)
      : Activity(NAME, renderer, mappedInput),
        epubPath(std::move(epubPath)),
        currentSpineIndex(0),
        currentPage(0),
        totalPagesInSpine(1),
        primaryMatchMethod(matchMethod),
        remoteMatchMethod(matchMethod),
        remoteProgress{},
        remotePosition{},
        localProgress{},
        localProgressDeferred(true),
        readerOrientation(readerOrientation) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
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
    SYNC_COMPLETE,
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
  DocumentMatchMethod primaryMatchMethod;
  DocumentMatchMethod remoteMatchMethod;

  State state = WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Loaded from saved reader progress after the remote TLS probe completes.
  KOReaderPosition localProgress;
  bool localProgressDeferred = false;
  bool restartBeforeNetwork = false;
  // The reader can use a book-specific orientation that its teardown restores
  // before this activity gets control. Keep that one value through the
  // lightweight network reboot so every sync screen matches the book.
  uint8_t readerOrientation = CrossPointSettings::ORIENTATION_COUNT;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;
  bool touchOverrideActive = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  bool consumeInitialConfirmRelease();
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  bool ensureLocalProgressLoaded();
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToReader();
};
