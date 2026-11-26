#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "BookOrbitBookmarkSync.h"
#include "BookOrbitHighlightSync.h"
#include "BookOrbitSyncClient.h"
#include "BookOrbitSyncReporter.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "util/BookOrbitSmartSync.h"

struct Rect;
struct ThemeMetrics;

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
 * 4. Exchange the reading stats, highlights and bookmarks on the same session
 *    (BookOrbitStatsUpload, BookOrbitHighlightSync, BookOrbitBookmarkSync)
 * 5. Show comparison and options (Apply/Upload), or let smart sync decide
 * 6. Apply or upload progress
 */
class BookOrbitSyncActivity final : public Activity, private BookOrbitSyncReporter {
 public:
  explicit BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                 int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                 KOReaderPosition localKoPos, std::string localChapterName,
                                 std::optional<uint16_t> currentParagraphIndex = std::nullopt, bool networkBoot = false,
                                 uint8_t readerOrientation = CrossPointSettings::ORIENTATION_COUNT,
                                 bool sleepSync = false)
      : Activity("BookOrbitSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        networkBoot(networkBoot),
        readerOrientation(readerOrientation),
        sleepSync(sleepSync),
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
  // No gesture may act on a device the user sees asleep.
  bool blocksGlobalInput() const override { return sleepSync; }

 private:
  // The integration harness reads these values from the logs (test/integration/harness/sim.py): keep the order.
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
    SYNC_COMPLETE,
    // The server knows the book but not this copy of it (positionIncompatible): offered once
    // the sync is over, on the way back to the book.
    UPDATE_OFFER
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
  // The reader can use a book-specific orientation that its teardown restores before
  // this activity gets control. Keep that one value through the lightweight network
  // reboot so every sync screen matches the book. (Mirrors KOReaderSyncActivity.)
  uint8_t readerOrientation = CrossPointSettings::ORIENTATION_COUNT;

  // The sync that runs behind the sleep screen (NetworkBootTarget::BOOKORBIT_SLEEP_SYNC).
  // Nothing is drawn, so the sleep screen stays on the panel; the whole attempt runs
  // inside the first loop() and ends in deep sleep. With nobody there to answer the choice
  // screen, progress follows smart sync's rules and is left alone when they cannot
  // decide. Power is the only input: the press that would wake the device cancels it.
  bool sleepSync = false;
  void runSleepSync();
  // True once the sync has been abandoned for a wake press; the device is rebooting.
  bool stopIfWoken();

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Progress of the phase under way (reading sessions, then highlights), drawn under
  // the status message (see BookOrbitSyncReporter). 0 total = no bar.
  size_t progressDone = 0;
  size_t progressTotal = 0;
  // A summary's lines under its title (the status message), in place of the count and bar.
  std::string summaryLines[2];

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;
  // The remote position's percentage recomputed in the local model (see
  // BookOrbitRemotePosition::localPercent): the sender's raw percentage comes from a
  // different engine and orders wrongly here.
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

  BookOrbitHighlightSync highlights{epubPath, documentHash};
  BookOrbitBookmarkSync bookmarks{epubPath, documentHash};

  // See KOReaderSyncActivity for why this tracking exists (esp_wifi_stop() during
  // performUpload() makes WiFi.getMode() unreliable for the onExit() decision).
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;
  bool touchOverrideActive = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  // The reading stats, then the highlights and bookmarks, on the session performSync()
  // opened. False when a wake press ended the sleep sync on the way.
  bool exchangeReadingData();
  void showExchangeSummaries();
  // statsAccepted: the reading-session events the server accepted this sync.
  void recordSweep(size_t statsAccepted);
  // Maps the server's position into local pages, then lets smart sync decide or asks.
  void resolveRemoteProgress();
  // True when smart sync settled the outcome without asking.
  bool decideAutomatically(const BookOrbitSmartSync::Position& local, const BookOrbitSmartSync::Position& remote);
  void performUpload();
  void uploadFromNoRemotePrompt();

  void setStatus(const char* message);
  void fail(const char* message);

  // BookOrbitSyncReporter, for the phases that run one batch per request.
  void beginPhase(const char* status, size_t total) override;
  void reportProgress(size_t done, size_t total) override;
  void endPhase() override;
  bool wakeRequested() const override;

  // The title band, whose height differs on touch devices: content below it
  // starts from this rect, never from metrics.headerHeight.
  Rect headerBandRect() const;
  void renderResult(const Rect& screen, const ThemeMetrics& metrics, int top);
  void drawHints(const char* confirm = "", const char* up = "", const char* down = "");
  void handleResultInput(bool backRequested);
  void handleNoRemoteProgressInput(bool backRequested);

  // The server answered the highlight or bookmark exchange with positionIncompatible: its
  // library file changed after this copy was made (a metadata write-back does that), so it
  // takes none of this copy's highlights or bookmarks. A manual sync offers to download the
  // library's copy over this one (BookOrbitCatalogLink::replacePath), which carries the local
  // state across (BookUpdateMigration); a sleep sync leaves the offer to the next manual one.
  bool documentIncompatible() const { return highlights.documentIncompatible() || bookmarks.documentIncompatible(); }
  // The catalog book to download, from the download index or, for a copy that reached the card
  // another way, from the server's match check (which resolves the copy's older hash). 0 when
  // neither knows it: the offer then has no Update.
  int64_t updateBookId = 0;
  bool updateOffered = false;
  void resolveUpdateBook();
  // Shows the offer and returns true, once per sync; false lets the sync end.
  bool offerBookUpdate();
  void startBookUpdate();
  void handleUpdateOfferInput(bool backRequested);

  bool consumeInitialConfirmRelease();
  void ensureEpubLoaded();
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToReader();
};
