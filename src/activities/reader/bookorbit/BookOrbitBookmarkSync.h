#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitBookmarks.h"

class Epub;
class GfxRenderer;

/**
 * One book's bookmarks exchange with BookOrbit, on the sync's one open session.
 *
 * The same phases as the highlights (see BookOrbitHighlightSync), with the identity
 * convention inverted (see BookOrbitBookmarks.h): applying a web bookmark MINTS a local
 * identity and reports it in the acknowledgment.
 *
 * epubPath and documentHash are the owning activity's, read at each step.
 */
class BookOrbitBookmarkSync {
 public:
  BookOrbitBookmarkSync(const std::string& epubPath, const std::string& documentHash)
      : epubPath(epubPath), documentHash(documentHash) {}

  // Copies the batch to upload and builds the key set, before the exchange.
  void prepare(bool serverAcceptsSidecar);
  // Uploads the batch and brings the server's changes down.
  void exchange();

  bool hasIncoming() const { return !incomingBookmarks.empty(); }
  // Streams each received bookmark's chapter to place it, in the lent framebuffer (see
  // placeReceivedBookmark): no RenderLock may be held by the caller.
  void applyIncoming(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer);

  uint16_t sent() const { return bookmarksSent; }
  uint16_t added() const { return bookmarksAdded; }
  uint16_t removed() const { return bookmarksRemoved; }
  bool changedAnything() const { return bookmarksSent > 0 || bookmarksAdded > 0 || bookmarksRemoved > 0; }
  // See BookOrbitHighlightSync::documentIncompatible().
  bool documentIncompatible() const { return incompatibleDocument; }

 private:
  const std::string& epubPath;
  const std::string& documentHash;

  std::vector<BookOrbitBookmark> pendingBookmarks;
  uint32_t pendingBookmarkWatermark = 0;
  std::vector<BookOrbitAnnotationKey> pendingBookmarkKeys;
  bool pendingBookmarkKeysComplete = false;
  BookOrbitSidecarState pendingBookmarkSidecar = BookOrbitSidecarState::Unstated;
  bool bookmarkStoreUnmarked = false;
  // See BookOrbitBookmarkStore::isFirstVersion(). The store moves on after this exchange; when it
  // has been through one before, this exchange first asks the server for every bookmark again.
  bool firstVersionStore = false;
  bool recoveringReceived = false;
  std::vector<BookOrbitIncomingBookmark> incomingBookmarks;
  uint16_t bookmarksSent = 0;
  uint16_t bookmarksAdded = 0;
  uint16_t bookmarksRemoved = 0;
  bool incompatibleDocument = false;
  // Their exchange still builds its key set as JSON nodes, and above this the set and its
  // JSON stop fitting beside a TLS session, so it is not sent and the server is told the set
  // is incomplete instead of being left to infer deletions. The highlight key set is written
  // straight into its request body and bounded by the heap instead.
  static constexpr size_t MAX_KEYS_PER_SYNC = 64;
};
