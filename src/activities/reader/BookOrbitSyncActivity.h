#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "BookOrbitAnnotations.h"
#include "BookOrbitSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

struct Rect;

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
                                 std::optional<uint16_t> currentParagraphIndex = std::nullopt, bool networkBoot = false)
      : Activity("BookOrbitSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        networkBoot(networkBoot),
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
    NO_CREDENTIALS,
    SYNC_COMPLETE
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  // True when this instance was created by the minimal network-boot resume (see
  // NetworkBootTarget::BOOKORBIT_SYNC); onEnter() then proceeds instead of restarting.
  bool networkBoot = false;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Reading-session drain progress, drawn under the status message. A long
  // backlog uploads 25 sessions per request, so without this the screen holds a
  // single unchanging line for a minute and looks stalled. 0 total = no bar.
  size_t statsUploaded = 0;
  size_t statsTotal = 0;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;
  // The remote position's percentage recomputed in the local model (see performSync):
  // the sender's raw percentage comes from a different engine and orders wrongly here.
  float remoteLocalPercent = 0.0f;

  // Local progress as KOReader-format wire data (pre-computed before Epub was released)
  KOReaderPosition localProgress;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Smart sync leaves its terminal screens on their own: set when an automatic
  // outcome lands, checked in loop(). 0 = no auto return armed.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();

  // The sync's one TLS session, created in performSync() and held through every request
  // that follows
  std::unique_ptr<BookOrbitSyncClient::Session> syncSession;

  // See KOReaderSyncActivity for why this tracking exists (esp_wifi_stop() during
  // performUpload() makes WiFi.getMode() unreliable for the onExit() decision).
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  // Returns how many queued reading-session events the server accepted this sync,
  // reported in the sweep record that closes the sync (see completeSweep).
  size_t uploadQueuedStats();

  // Highlights are copied out of the clipping store one batch at a time and the store is
  // unloaded before the request runs: the resident store is ~20KB, a batch ~6KB, and only
  // the batch may stay resident beside the sync's open TLS session. One batch per sync;
  // the watermark makes the rest follow on later syncs.
  void prepareAnnotationBatch();
  void uploadAnnotationBatch();
  // Runs once the epub is loaded: placing an incoming annotation needs the spine to parse its
  // xpointer against. Loads the clipping store beside the open TLS session -- a failed load on
  // a tight heap leaves the highlights pending for the next sync -- and acknowledges what
  // landed on that same session.
  void applyIncomingAnnotations();
  std::vector<BookOrbitIncomingAnnotation> incomingAnnotations;

  // Bookmarks follow the same three phases on the same session. Their identity convention is
  // inverted (see BookOrbitBookmarks.h): applying a web bookmark MINTS a local identity and
  // reports it in the acknowledgment.
  void prepareBookmarkBatch();
  void uploadBookmarkBatch();
  void applyIncomingBookmarks();
  std::vector<BookOrbitBookmark> pendingBookmarks;
  uint32_t pendingBookmarkWatermark = 0;
  std::vector<BookOrbitAnnotationKey> pendingBookmarkKeys;
  bool pendingBookmarkKeysComplete = false;
  std::vector<BookOrbitIncomingBookmark> incomingBookmarks;
  uint16_t bookmarksSent = 0;
  uint16_t bookmarksAdded = 0;
  uint16_t bookmarksRemoved = 0;
  // Counted so the screen can say what the exchange did. Without it the step is invisible: the
  // only way to know whether highlights synced was to read the serial log.
  uint16_t annotationsSent = 0;
  uint16_t annotationsAdded = 0;
  uint16_t annotationsRemoved = 0;
  // The server's answer on whether it recognises this document at all, taken from the
  // annotation exchange (which always runs); the sweep record reports it as booksMatched.
  bool documentUnmatched = false;
  std::vector<BookOrbitAnnotation> pendingAnnotations;
  uint32_t pendingAnnotationWatermark = 0;
  // Fixed-width rows rather than a container of strings, so the whole set is one allocation.
  std::vector<BookOrbitAnnotationKey> pendingAnnotationKeys;
  bool pendingKeysComplete = false;
  // Above this, the key set and its JSON stop fitting beside a TLS session, so it is not sent
  // and the server is told the set is incomplete instead of being left to infer deletions.
  static constexpr size_t MAX_KEYS_PER_SYNC = 64;
  // The title band, whose height differs on touch devices: content below it
  // starts from this rect, never from metrics.headerHeight.
  Rect headerBandRect() const;

  bool consumeInitialConfirmRelease();
  void ensureEpubLoaded();
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToReader();
};
