#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "BookOrbitAnnotations.h"

class BookOrbitSyncReporter;
class Epub;

/**
 * One book's highlights exchange with BookOrbit, on the sync's one open session.
 *
 * Three steps. The first request carries the key set and brings the server's changes
 * down (exchangeKeys); those are applied (drainIncoming), then this device's new
 * highlights go up one batch per request (sendBacklog), as the reading stats do. Each
 * batch is copied out of the clipping store and the store unloaded before its request
 * runs: the resident store is ~20KB, a batch ~6KB, and only the batch may stay resident
 * beside the open TLS session.
 *
 * epubPath and documentHash are the owning activity's, read at each step.
 */
class BookOrbitHighlightSync {
 public:
  BookOrbitHighlightSync(const std::string& epubPath, const std::string& documentHash)
      : epubPath(epubPath), documentHash(documentHash) {}

  // Counts what waits to go up and builds the key set, before the first request.
  void prepare(bool serverAcceptsSidecar);
  // The first request. False when it failed or the server does not know the document:
  // nothing more is exchanged, and the next sync retries.
  bool exchangeKeys();

  bool hasIncoming() const { return !incomingAnnotations.empty(); }
  // Without the epub nothing received can be placed; it stays pending on the server.
  void discardIncoming() { incomingAnnotations.clear(); }
  // Applies the batch in hand, then pulls and applies the rest one batch per request,
  // until the server offers nothing more this device can take. Placing an incoming
  // annotation needs the spine to parse its xpointer against.
  void drainIncoming(const Epub& epub, BookOrbitSyncReporter& reporter);
  void sendBacklog(BookOrbitSyncReporter& reporter);

  // The server's answer on whether it recognises this document at all, taken from the
  // annotation exchange (which always runs); the sweep record reports it as booksMatched.
  bool documentUnmatched() const { return unmatchedDocument; }
  uint16_t sent() const { return annotationsSent; }
  uint16_t added() const { return annotationsAdded; }
  uint16_t removed() const { return annotationsRemoved; }
  uint32_t deletedOnServer() const { return annotationsDeletedOnServer; }
  bool changedAnything() const {
    return annotationsSent > 0 || annotationsAdded > 0 || annotationsRemoved > 0 || annotationsDeletedOnServer > 0;
  }

 private:
  bool prepareNextAnnotationBatch(uint32_t watermark);
  // Loads the clipping store beside the open TLS session -- a failed load on a tight heap
  // leaves the highlights pending for the next sync -- and acknowledges what landed on that
  // same session. Returns false when the batch could not be stored or acknowledged;
  // outLanded counts what the server was told landed.
  bool applyIncomingAnnotations(const Epub& epub, size_t& outLanded);

  const std::string& epubPath;
  const std::string& documentHash;

  std::vector<BookOrbitIncomingAnnotation> incomingAnnotations;
  // What the last pull offered (the batch in hand included) and whether it said more waits.
  size_t incomingOffered = 0;
  bool incomingMorePending = false;
  // Server changes this sync could not place: the server offers them until acknowledged, and
  // left in the pulls they would take a batch's place every round.
  std::vector<uint32_t> unplacedAnnotationIds;
  // One per highlight a book can hold, in batches: a safety net, not a limit a book reaches.
  static constexpr size_t MAX_INCOMING_ROUNDS = 256 / BOOKORBIT_ANNOTATION_BATCH;

  // Counted so the screen can say what the exchange did. Without it the step is invisible: the
  // only way to know whether highlights synced was to read the serial log.
  uint16_t annotationsSent = 0;
  uint16_t annotationsAdded = 0;
  uint16_t annotationsRemoved = 0;
  // Deleted here and, through the key set, on the server too.
  uint32_t annotationsDeletedOnServer = 0;
  bool unmatchedDocument = false;

  std::vector<BookOrbitAnnotation> pendingAnnotations;
  uint32_t pendingAnnotationWatermark = 0;
  // The store's watermark when the sync began: what is above it and sendable goes up.
  uint32_t annotationWatermark = 0;
  // Every highlight waiting to go, counted by prepare() for the progress shown.
  size_t annotationsToSend = 0;
  // Fixed-width rows rather than a container of strings, so the whole set is one allocation.
  std::vector<BookOrbitAnnotationKey> pendingAnnotationKeys;
  bool pendingKeysComplete = false;
  BookOrbitSidecarState pendingAnnotationSidecar = BookOrbitSidecarState::Unstated;
  // A readable store whose watermark is still 0: the exchange that goes through marks it (see
  // BookOrbitAnnotationStore::markExchanged), so the next sync can report it as known.
  bool annotationStoreUnmarked = false;
  // Left beside the key set when it is built (see prepare).
  static constexpr size_t KEY_SET_HEAP_MARGIN = 4096;
};
