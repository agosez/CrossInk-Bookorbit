#include "BookOrbitHighlightSync.h"

#include <Arduino.h>
#include <Epub.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "BookOrbitAnnotationStore.h"
#include "BookOrbitKeySet.h"
#include "BookOrbitSyncClient.h"
#include "BookOrbitSyncReporter.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "ProgressMapper.h"
#include "util/BookContentId.h"

namespace {
/**
 * The clipping a record uploads (CLIPPINGS must be loaded), with its index in the store, or
 * nullptr when there is none to upload.
 */
const Clipping* sendableClipping(const BookOrbitAnnotationRecord& record, size_t& outIndex) {
  const BookOrbitClippingRef wanted{record.timestamp, record.spineIndex, record.paragraphIndex};
  for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
    const Clipping* candidate = CLIPPINGS.clippingAt(i);
    if (candidate &&
        BookOrbitClippingRef{candidate->timestamp, candidate->spineIndex, candidate->paragraphIndex} == wanted) {
      outIndex = i;
      // Received from the server (applyIncomingAnnotations stores those with no word range; a
      // highlight made here always covers a word). The server holds it already, and with more
      // than this device can show: sent back, BookOrbit 3.5.0's field-by-field merge reads the
      // missing note as removed and the plain style as changed, and the range shrinks to pos0.
      return candidate->wordCount == 0 ? nullptr : candidate;
    }
  }
  return nullptr;  // deleted locally; the key list will tell the server
}

/**
 * A timestamp for a clipping received in this chapter and paragraph (CLIPPINGS must be loaded).
 *
 * A clipping's record joins back to it by timestamp, chapter and paragraph (BookOrbitClippingRef),
 * and the timestamp counts seconds since boot: a second highlight received for the same paragraph
 * in the same second would take the first one's record over, and that one would never be reported
 * again. A lost copy offered back in full, or a web reader's run of highlights in one paragraph,
 * makes that likely. So each takes the latest second not already used there, counting back from
 * now: a second this boot has passed cannot be taken by a highlight made later in it.
 */
uint32_t freeClippingTimestamp(const uint16_t spine, const uint16_t paragraph) {
  const auto taken = [&](const uint32_t timestamp) {
    for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
      const Clipping* clipping = CLIPPINGS.clippingAt(i);
      if (clipping && BookOrbitClippingRef{clipping->timestamp, clipping->spineIndex, clipping->paragraphIndex} ==
                          BookOrbitClippingRef{timestamp, spine, paragraph}) {
        return true;
      }
    }
    return false;
  };
  const uint32_t now = std::max<uint32_t>(1, static_cast<uint32_t>(millis() / 1000UL));
  for (uint32_t timestamp = now; timestamp >= 1; timestamp--) {
    if (!taken(timestamp)) return timestamp;
  }
  // A network boot is only seconds old: past those, the store's own bound ends the search.
  uint32_t timestamp = now + 1;
  while (taken(timestamp)) timestamp++;
  return timestamp;
}

/**
 * Copy the next upload batch out of the stores (CLIPPINGS must be loaded): the highlights above
 * `watermark`, oldest identity first.
 *
 * An accepted batch moves the watermark to its newest datetime, so the batch has to be the oldest
 * of what is left. The store keeps insertion order instead, which a clock set back between two
 * highlights breaks, and a highlight left outside a batch below its newest one would never be
 * sent. The highlights sharing a datetime go in one batch for the same reason.
 *
 * @return true when the batch stopped on a bound with records still above it
 */
bool collectAnnotationBatch(const std::vector<BookOrbitAnnotationRecord>& records, const uint32_t watermark,
                            std::vector<BookOrbitAnnotation>& out, uint32_t& outWatermark) {
  size_t batchTextBytes = 0;
  uint32_t datetime = watermark;
  for (;;) {
    // The next datetime above the last one taken. One scan per datetime rather than a sort: a
    // batch spans only a few of them, and std::sort over these records costs ~1 KB of C3 flash.
    bool found = false;
    uint32_t next = 0;
    for (const BookOrbitAnnotationRecord& record : records) {
      if (record.identityEpoch > datetime && (!found || record.identityEpoch < next)) {
        next = record.identityEpoch;
        found = true;
      }
    }
    if (!found) return false;
    if (out.size() >= BOOKORBIT_ANNOTATION_BATCH || batchTextBytes >= BOOKORBIT_ANNOTATION_BATCH_TEXT_BYTES) {
      return true;
    }
    datetime = next;

    for (const BookOrbitAnnotationRecord& record : records) {
      if (record.identityEpoch != datetime) continue;
      size_t clippingIndex = 0;
      const Clipping* clipping = sendableClipping(record, clippingIndex);
      if (!clipping) continue;

      BookOrbitAnnotation annotation;
      // identityEpoch was minted from WallClock when the highlight was created; the server
      // validates the formatted datetime strictly and drops the entry otherwise.
      if (!bookOrbitFormatDatetime(record.identityEpoch, annotation.datetime)) continue;

      if (!CLIPPINGS.readClippingText(clippingIndex, annotation.text) || annotation.text.empty()) continue;
      annotation.pos0 = record.pos0;
      annotation.pos1 = record.pos1;
      annotation.chapter = clipping->chapterTitle;
      annotation.pageno = static_cast<uint16_t>(clipping->startPage + 1);
      batchTextBytes += annotation.text.size();
      out.push_back(std::move(annotation));
      outWatermark = datetime;
    }
  }
}
}  // namespace

void BookOrbitHighlightSync::prepare(const bool serverAcceptsSidecar) {
  pendingAnnotations.clear();
  pendingAnnotationWatermark = 0;
  annotationsToSend = 0;
  pendingAnnotationSidecar = BookOrbitSidecarState::Unstated;
  annotationStoreUnmarked = false;

  LOG_INF("BookOrbit", "Preparing highlights (free=%u maxAlloc=%u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitAnnotationRecord> records;
  uint32_t watermark = 0;
  // A readable store is the proof this book has synced highlights before, even one holding
  // zero records (see the completeness rule below). One read for the records and the watermark:
  // each extra read held another full copy of the records beside these.
  const bool syncedHereBefore = BookOrbitAnnotationStore::readAll(stateDir, records, &watermark);
  // The store is loaded only for as long as it takes to copy the batch out, and unloaded
  // before returning: only the small batch stays resident beside the open TLS session.
  if (!CLIPPINGS.loadForSync(epubPath, 0)) {
    LOG_ERR("BookOrbit", "Could not read clippings; highlights will not sync");
    return;
  }
  const size_t localClippingCount = CLIPPINGS.clippingCount();

  // Deleting a clipping leaves its record behind, so drop the orphans before anything reads the
  // set: they would otherwise fill the file and, worse, describe annotations to the server that
  // this device no longer holds.
  std::vector<BookOrbitClippingRef> liveClippings;
  liveClippings.reserve(localClippingCount);
  for (size_t i = 0; i < localClippingCount; i++) {
    const Clipping* clipping = CLIPPINGS.clippingAt(i);
    if (clipping && clipping->timestamp != 0) {
      liveClippings.push_back({clipping->timestamp, clipping->spineIndex, clipping->paragraphIndex});
    }
  }
  BookOrbitAnnotationStore::retain(stateDir, watermark, records, liveClippings);

  for (const BookOrbitAnnotationRecord& record : records) {
    size_t clippingIndex = 0;
    if (record.identityEpoch > watermark && sendableClipping(record, clippingIndex)) annotationsToSend++;
  }
  annotationWatermark = watermark;

  CLIPPINGS.unload();

  // The key set is what lets the server notice highlights deleted on the device: it compares
  // its own set against this one. Sending it is optional, and skipped when the heap cannot hold
  // it: 53 bytes a key here, ~70 more in the request body (see exchangeKeys).
  // keysComplete=false is exactly the flag for that case: the server then skips deletion
  // processing rather than reading the gap as deletions.
  pendingAnnotationKeys.clear();
  pendingKeysComplete = false;
  // Complete means every local highlight is described here -- not that the two counts happen to
  // match. A highlight still waiting for its position is absent from the store, and reporting the
  // set as complete without it would have the server read the gap as a deletion and erase it,
  // losing data on the server to describe a device that never said anything of the sort.
  //
  // An empty set IS reported as complete: deleting the last local highlight must propagate, and
  // it is safe because the server scopes deletion detection to this device's own sync states
  // (findStatesForDeviceBook) -- an empty set deletes exactly what this device provably held,
  // never a web highlight it was yet to receive. The reference plugin reports empty-complete too.
  //
  // ...but only once a readable store proves this book has synced highlights before. The
  // store is keyed by the book's content hash, so a moved or renamed file keeps it; a missing
  // or unreadable store means "never synced this book" or "history lost" (a wiped card, a
  // fresh device), never "the user deleted everything" -- deleting a highlight requires
  // holding it, which requires the store. Claiming completeness over a lost store would have
  // the server erase every highlight this device ever acked. The guard only makes the loss
  // non-destructive; bringing the highlights back takes the "new" sidecar state below, which
  // only a server that accepts it and a book with no highlight left here can use. Deletion
  // reporting resumes on its own once highlights sync again.
  const bool everyClippingHasAPosition = records.size() >= localClippingCount;
  const bool keySetFits =
      ESP.getMaxAllocHeap() >= records.size() * sizeof(BookOrbitAnnotationKey) + KEY_SET_HEAP_MARGIN;
  if (!syncedHereBefore) {
    LOG_INF("BookOrbit", "No highlight sync history for this book; deletions will not propagate this sync");
  } else if (everyClippingHasAPosition && keySetFits) {
    pendingKeysComplete = BookOrbitKeySet::build(records, pendingAnnotationKeys);
  } else if (!everyClippingHasAPosition) {
    LOG_INF("BookOrbit", "%u of %u highlights carry a position; deletions will not propagate yet",
            (unsigned)records.size(), (unsigned)localClippingCount);
  } else {
    LOG_ERR("BookOrbit", "No heap for a key set of %u highlights (free=%u maxAlloc=%u); deletions will not propagate",
            (unsigned)records.size(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }

  // Without it, BookOrbit 3.5.0 reads a complete key set that keeps none of the highlights this
  // device had applied as a lost copy, and hands them all back: deleting every synced highlight
  // of a book would undo itself on the next sync.
  pendingAnnotationSidecar =
      bookOrbitSidecarState(syncedHereBefore, watermark, localClippingCount, serverAcceptsSidecar);
  annotationStoreUnmarked = syncedHereBefore && watermark == 0;

  LOG_INF("BookOrbit", "%u highlight(s) to send, %u key(s) (watermark=%lu, sidecar=%d)", (unsigned)annotationsToSend,
          (unsigned)pendingAnnotationKeys.size(), static_cast<unsigned long>(watermark),
          static_cast<int>(pendingAnnotationSidecar));
}

bool BookOrbitHighlightSync::exchangeKeys() {
  // Deliberately not skipped when there is nothing to say. This request is the only thing that
  // brings the server's own highlight changes down, so skipping it would mean a device that has
  // said everything it has to say never hears anything back.
  bool unmatched = false;
  bool morePending = false;
  const BookOrbitAnnotationKeys keys{pendingAnnotationKeys.empty() ? nullptr : pendingAnnotationKeys.data(),
                                     pendingAnnotationKeys.size(), pendingKeysComplete, pendingAnnotationSidecar};
  BookOrbitSyncClient::Error result = BookOrbitSyncClient::exchangeAnnotations(
      documentHash, SETTINGS.getEffectiveDeviceName(), keys, nullptr, 0, unmatched, &incomingAnnotations, &morePending,
      nullptr, &incomingOffered, &annotationsDeletedOnServer);
  if (result == BookOrbitSyncClient::LOW_MEMORY && keys.count > 0) {
    // The body holds the whole key set at once. Without it this sync still brings the server's
    // changes down and sends what is new; only deletions made here wait for a sync with room.
    LOG_ERR("BookOrbit", "No heap for a %u-key exchange; exchanging without the key set", (unsigned)keys.count);
    const BookOrbitAnnotationKeys withoutKeys{nullptr, 0, false, pendingAnnotationSidecar};
    result = BookOrbitSyncClient::exchangeAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), withoutKeys,
                                                      nullptr, 0, unmatched, &incomingAnnotations, &morePending,
                                                      nullptr, &incomingOffered, &annotationsDeletedOnServer);
  }
  incompatibleDocument = BookOrbitSyncClient::lastPositionIncompatible;
  LOG_INF("BookOrbit",
          "Highlight exchange result=%d (http=%d, unmatched=%d, incompatible=%d, keys=%u, deleted on server=%lu)",
          static_cast<int>(result), BookOrbitSyncClient::lastHttpCode, unmatched ? 1 : 0, incompatibleDocument ? 1 : 0,
          (unsigned)keys.count, static_cast<unsigned long>(annotationsDeletedOnServer));
  // The sweep's booksMatched: an incompatible copy's book is one the server knows.
  unmatchedDocument = unmatched && !incompatibleDocument;
  // The key set goes out with the first request only.
  pendingAnnotationKeys.clear();
  pendingAnnotationKeys.shrink_to_fit();
  if (result != BookOrbitSyncClient::OK || unmatched) return false;  // retried on the next sync

  if (annotationStoreUnmarked) BookOrbitAnnotationStore::markExchanged(BookContentId::bookStateDir(epubPath));

  // The server converts a web highlight's position lazily, one budget per request, and a
  // highlight converted during THIS request only ships on the next one -- with no signal in the
  // response (skippedNoPosition counts conversions beyond the budget, not the ones that
  // happened). The first extra round is therefore unconditional: it is the only way a freshly
  // created web highlight arrives on the same sync that converted it, instead of surfacing as
  // "my highlight needed two syncs". The second round runs only when the server says more is
  // pending. The key set went out above and is not repeated.
  const BookOrbitAnnotationKeys noKeys{nullptr, 0, false};
  for (int round = 0;
       round < 2 && incomingAnnotations.size() < BOOKORBIT_ANNOTATION_BATCH && (round == 0 || morePending); round++) {
    morePending = false;
    bool roundUnmatched = false;
    if (BookOrbitSyncClient::exchangeAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), noKeys, nullptr, 0,
                                                 roundUnmatched, &incomingAnnotations, &morePending, nullptr,
                                                 &incomingOffered) != BookOrbitSyncClient::OK ||
        roundUnmatched) {
      break;
    }
    LOG_INF("BookOrbit", "Pulled again: %u change(s) total", (unsigned)incomingAnnotations.size());
  }
  incomingMorePending = morePending;
  return true;
}

void BookOrbitHighlightSync::sendBacklog(BookOrbitSyncReporter& reporter) {
  if (annotationsToSend == 0) return;
  reporter.beginPhase(tr(STR_SENDING_HIGHLIGHTS), annotationsToSend);

  // One batch per request on the session already open, as the reading stats are drained: a
  // book highlighted offline used to take one sync per batch. Each batch moves the watermark
  // once accepted, so a failure or a wake leaves the rest to the next sync.
  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  const BookOrbitAnnotationKeys noKeys{nullptr, 0, false};
  uint32_t sentUpTo = annotationWatermark;
  bool more = true;
  while (more) {
    if (reporter.wakeRequested()) break;
    more = prepareNextAnnotationBatch(sentUpTo);
    if (pendingAnnotations.empty()) break;
    bool unmatched = false;
    const BookOrbitSyncClient::Error result =
        BookOrbitSyncClient::exchangeAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), noKeys,
                                                 pendingAnnotations.data(), pendingAnnotations.size(), unmatched);
    if (result != BookOrbitSyncClient::OK || unmatched) {
      LOG_ERR("BookOrbit", "Highlight batch failed (result=%d, http=%d); the rest follows on the next sync",
              static_cast<int>(result), BookOrbitSyncClient::lastHttpCode);
      break;
    }
    annotationsSent = static_cast<uint16_t>(annotationsSent + pendingAnnotations.size());
    LOG_INF("BookOrbit", "Highlight batch accepted: %u of %u sent", (unsigned)annotationsSent,
            (unsigned)annotationsToSend);
    reporter.reportProgress(annotationsSent, annotationsToSend);
    if (!BookOrbitAnnotationStore::advanceWatermark(stateDir, pendingAnnotationWatermark)) {
      LOG_ERR("BookOrbit", "Highlights uploaded but the watermark did not advance; they will be re-sent");
      break;
    }
    sentUpTo = pendingAnnotationWatermark;
  }
  pendingAnnotations.clear();
  pendingAnnotations.shrink_to_fit();
  reporter.endPhase();
}

bool BookOrbitHighlightSync::prepareNextAnnotationBatch(const uint32_t watermark) {
  pendingAnnotations.clear();
  pendingAnnotationWatermark = 0;

  // Read for each batch rather than kept: the records and the clipping store are exactly what
  // must not stay resident beside the session (see prepare). The orphans were dropped then,
  // and the key set went out with the first request.
  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitAnnotationRecord> records;
  if (!BookOrbitAnnotationStore::readAll(stateDir, records)) {
    LOG_ERR("BookOrbit", "Could not read the highlight store again; the rest follows on the next sync");
    return false;
  }
  if (!CLIPPINGS.loadForSync(epubPath, 0)) {
    LOG_ERR("BookOrbit", "Could not read clippings; the rest follows on the next sync");
    return false;
  }
  const bool more = collectAnnotationBatch(records, watermark, pendingAnnotations, pendingAnnotationWatermark);
  CLIPPINGS.unload();
  return more;
}

void BookOrbitHighlightSync::drainIncoming(const Epub& epub, BookOrbitSyncReporter& reporter) {
  if (incomingAnnotations.empty()) return;
  // The total is what the server offers, which understates a backlog longer than its page: it
  // is corrected by each pull.
  size_t landed = 0;
  size_t total = std::max(incomingOffered, incomingAnnotations.size());
  bool more = incomingMorePending || incomingOffered > incomingAnnotations.size();
  reporter.beginPhase(tr(STR_RECEIVING_HIGHLIGHTS), total);

  // The batch in hand first, then one pull per batch on the session already open, as the
  // uploads go: the server keeps offering what is not acknowledged, so each acknowledged batch
  // makes room for the next one. What cannot be placed is set aside rather than offered again.
  const BookOrbitAnnotationKeys noKeys{nullptr, 0, false};
  for (size_t round = 0; round < MAX_INCOMING_ROUNDS && !incomingAnnotations.empty(); round++) {
    const size_t setAsideBefore = unplacedAnnotationIds.size();
    size_t roundLanded = 0;
    if (!applyIncomingAnnotations(epub, roundLanded)) break;
    landed += roundLanded;
    reporter.reportProgress(landed, std::max(total, landed));
    // Nothing landed and nothing set aside: the next pull would offer this very batch again.
    if (!more || (roundLanded == 0 && unplacedAnnotationIds.size() == setAsideBefore)) break;
    if (reporter.wakeRequested()) break;

    bool unmatched = false;
    size_t offered = 0;
    if (BookOrbitSyncClient::exchangeAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), noKeys, nullptr, 0,
                                                 unmatched, &incomingAnnotations, &more, &unplacedAnnotationIds,
                                                 &offered) != BookOrbitSyncClient::OK ||
        unmatched) {
      LOG_ERR("BookOrbit", "Highlight pull failed (http=%d); the rest arrives next sync",
              BookOrbitSyncClient::lastHttpCode);
      break;
    }
    total = landed + offered;
    more = more || offered > incomingAnnotations.size();
  }
  if (!incomingAnnotations.empty()) {
    LOG_INF("BookOrbit", "%u server highlight change(s) left for the next sync", (unsigned)incomingAnnotations.size());
    incomingAnnotations.clear();
  }
  reporter.endPhase();
}

bool BookOrbitHighlightSync::applyIncomingAnnotations(const Epub& epub, size_t& outLanded) {
  outLanded = 0;
  if (incomingAnnotations.empty()) return true;

  // This loads the clipping store (~20KB) beside the open TLS session; if that load fails on a
  // tight heap the highlights stay pending for the next sync -- a clean retry, where closing the
  // session here would force the progress upload back onto an unaffordable fresh handshake. The
  // acknowledgment that follows rides the kept session, so no handshake needs room beside the store.
  std::vector<BookOrbitAckEntry> appliedIds;
  std::vector<BookOrbitAckEntry> deletedIds;
  const size_t additions =
      static_cast<size_t>(std::count_if(incomingAnnotations.begin(), incomingAnnotations.end(),
                                        [](const BookOrbitIncomingAnnotation& incoming) { return !incoming.deleted; }));
  if (!CLIPPINGS.loadForSync(epubPath, additions)) {
    LOG_ERR("BookOrbit", "Could not open clippings; server-side highlights stay pending");
    return false;
  }

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitAnnotationRecord> records;
  BookOrbitAnnotationStore::readAll(stateDir, records);
  // The positions of what lands, written in one go once records and the clipping store are
  // released: a write per highlight held a second copy of every record beside both.
  std::vector<BookOrbitAnnotationRecord> received;
  received.reserve(additions);

  bool storeChanged = false;
  size_t addedCount = 0;
  size_t removedCount = 0;
  for (const BookOrbitIncomingAnnotation& incoming : incomingAnnotations) {
    // A deletion carries no position, only the key this device reported for it. Resolving that
    // key back through the annotation store names the exact highlight, which beats guessing from
    // text: two identical quotations in one chapter would otherwise be indistinguishable.
    if (incoming.deleted) {
      BookOrbitClippingRef target;
      bool found = false;
      for (const BookOrbitAnnotationRecord& record : records) {
        char datetime[20] = {};
        char key[BookOrbitAnnotationKey::DIGEST_SIZE] = {};
        if (!bookOrbitFormatDatetime(record.identityEpoch, datetime) ||
            !bookOrbitAnnotationKey(datetime, record.pos0.c_str(), key)) {
          continue;
        }
        if (incoming.key == key) {
          target = {record.timestamp, record.spineIndex, record.paragraphIndex};
          found = true;
          break;
        }
      }

      bool removedHere = false;
      if (found) {
        for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
          const Clipping* clipping = CLIPPINGS.clippingAt(i);
          if (!clipping ||
              !(BookOrbitClippingRef{clipping->timestamp, clipping->spineIndex, clipping->paragraphIndex} == target)) {
            continue;
          }
          removedHere = CLIPPINGS.removeClippingAt(i);
          break;
        }
      }
      if (removedHere) {
        storeChanged = true;
        removedCount++;
      } else {
        LOG_INF("BookOrbit", "Server deletion %lu matched no local highlight (key=%s)",
                static_cast<unsigned long>(incoming.serverId), incoming.key.c_str());
      }
      // Acknowledged either way: already gone locally is the same outcome, and withholding it
      // would have the server offer the same deletion on every sync forever.
      deletedIds.push_back({incoming.serverId, 0});
      continue;
    }

    // DocFragment[N] gives the chapter and p[M] the paragraph, which is all a clipping needs to
    // be found again: the reader locates a clipping by its text whenever the stored range does
    // not match the current layout (see findClippingTextOnPageLoose), so no page or word
    // coordinates have to be invented here.
    int spineIndex = 0;
    uint16_t paragraphHint = UINT16_MAX;
    if (!ProgressMapper::parseXPointerLocation(incoming.pos0, spineIndex, paragraphHint) || spineIndex < 0 ||
        spineIndex >= epub.getSpineItemsCount()) {
      LOG_ERR("BookOrbit", "Cannot place server annotation %lu from %s", static_cast<unsigned long>(incoming.serverId),
              incoming.pos0.c_str());
      unplacedAnnotationIds.push_back(incoming.serverId);
      continue;  // left unacknowledged, so the server offers it again
    }
    const uint16_t spine = static_cast<uint16_t>(spineIndex);

    // Matched on chapter and text, never on the paragraph hint. The server normalizes the
    // xpointers it stores (crengine omits the index of a single child), so a highlight of ours
    // comes back with a path that no longer matches the one we sent -- and the paragraph number
    // read out of it is a sibling index, not the count CrossInk assigns. The text is the only
    // identity both sides agree on, and it is also what the reader draws from.
    if (incoming.text.empty()) {
      unplacedAnnotationIds.push_back(incoming.serverId);
      continue;
    }

    // A highlight this device already holds, named by the identity it reported or received: since
    // BookOrbit 3.5.0 keeps its state per device AND book file, the first exchange after a server
    // upgrade offers every highlight of the book back, this device's own included. The datetime is
    // the server's copy of ours, byte for byte; the position may not be (see above), so a datetime
    // that names one highlight in this chapter is enough. The text match after this one cannot
    // tell those apart from new highlights when the server's text was cut short
    // (BOOKORBIT_ANNOTATION_TEXT_MAX) or differs in any way.
    const auto findLocalByIdentity = [&](size_t& outIndex) {
      const BookOrbitAnnotationRecord* match = nullptr;
      size_t sameDatetimeInChapter = 0;
      for (const BookOrbitAnnotationRecord& record : records) {
        char datetime[20] = {};
        if (!bookOrbitFormatDatetime(record.identityEpoch, datetime) || std::strcmp(datetime, incoming.datetime) != 0) {
          continue;
        }
        if (record.pos0 == incoming.pos0) {
          match = &record;
          sameDatetimeInChapter = 1;
          break;
        }
        if (record.spineIndex == spine) {
          match = &record;
          sameDatetimeInChapter++;
        }
      }
      if (!match || sameDatetimeInChapter != 1) return false;
      const BookOrbitClippingRef ref{match->timestamp, match->spineIndex, match->paragraphIndex};
      for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
        const Clipping* clipping = CLIPPINGS.clippingAt(i);
        if (clipping &&
            BookOrbitClippingRef{clipping->timestamp, clipping->spineIndex, clipping->paragraphIndex} == ref) {
          outIndex = i;
          return true;
        }
      }
      return false;
    };

    const auto findLocalByText = [&](size_t& outIndex) {
      for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
        const Clipping* clipping = CLIPPINGS.clippingAt(i);
        if (!clipping || clipping->spineIndex != spine) continue;
        std::string localText;
        if (!CLIPPINGS.readClippingText(i, localText) || localText != incoming.text) continue;
        outIndex = i;
        return true;
      }
      return false;
    };

    size_t existingIndex = 0;
    if (findLocalByIdentity(existingIndex) || findLocalByText(existingIndex)) {
      appliedIds.push_back({incoming.serverId, incoming.version});  // nothing to do, but it did land
      continue;
    }

    // Page and word coordinates are deliberately left at zero (the store clamps pageCount to 1).
    // findClippingStoredRangeOnPage rejects wordCount==0, so these coordinates never place the
    // highlight: the reader locates it by its text, rather than trusting coordinates this
    // device never measured. Zero is also the honest layout signature here.
    const size_t newIndex = CLIPPINGS.clippingCount();
    // No table selection either: a server annotation carries text, not a cell coordinate.
    const auto addResult = CLIPPINGS.addClipping(spine, 0, 0, 0, 0, 0, 0, incoming.chapter.c_str(), paragraphHint,
                                                 incoming.text, /*tableSelection=*/UINT16_MAX, /*layoutSignature=*/0,
                                                 freeClippingTimestamp(spine, paragraphHint));
    if (addResult != ClippingStore::AddResult::Added) {
      LOG_ERR("BookOrbit", "Could not store server annotation %lu (result=%d)",
              static_cast<unsigned long>(incoming.serverId), static_cast<int>(addResult));
      unplacedAnnotationIds.push_back(incoming.serverId);
      continue;  // unacknowledged: a full store should not silently swallow it
    }
    storeChanged = true;
    const Clipping* stored = CLIPPINGS.clippingAt(newIndex);
    addedCount++;
    appliedIds.push_back({incoming.serverId, incoming.version});

    // Record its position too, so the next sync reports it in the key set as ours rather than
    // offering it back as a new highlight.
    if (stored) {
      BookOrbitAnnotationRecord record;
      record.timestamp = stored->timestamp;
      // The server's datetime, not the moment this clipping was written: that is what it hashes
      // its identity from, and keying on the local moment made it re-offer this highlight every
      // sync and never report its deletion.
      if (!bookOrbitParseDatetime(incoming.datetime, record.identityEpoch)) {
        record.identityEpoch = stored->timestamp;
        LOG_ERR("BookOrbit", "Server datetime \"%s\" unreadable; this highlight may be re-offered", incoming.datetime);
      }
      record.spineIndex = spine;
      record.paragraphIndex = paragraphHint;
      // Stored exactly as the server sent it, so the key we report next sync is the one it
      // computed. Re-deriving it here would produce a different hash and have our own highlight
      // offered back to us on every sync.
      record.pos0 = incoming.pos0;
      record.pos1 = incoming.pos0;
      received.push_back(std::move(record));
    }
  }

  std::vector<BookOrbitAnnotationRecord>().swap(records);
  if (storeChanged && !CLIPPINGS.saveToFile()) {
    LOG_ERR("BookOrbit", "Failed to save received highlights");
    CLIPPINGS.unload();
    return false;  // nothing acknowledged: the server keeps them and the next sync retries
  }
  CLIPPINGS.unload();
  if (!received.empty() && !BookOrbitAnnotationStore::putAll(stateDir, received)) {
    LOG_ERR("BookOrbit", "Could not record the position of every received highlight");
  }
  // A store this apply just started holds exactly what the exchange delivered, so it counts as
  // exchanged: deleting those highlights on the next sync must not read as a lost copy.
  if (addedCount > 0) BookOrbitAnnotationStore::markExchanged(stateDir);

  // Only what changed here counts: a highlight already here is acknowledged too, and after a
  // server upgrade that is every highlight of the book; so is a deletion of one already gone.
  // The watermark is left alone. Received highlights
  // are never uploaded (see prepare), and a server datetime can be ahead of this
  // device's clock: moving the watermark up to it would hide the next highlights made here.
  annotationsAdded = static_cast<uint16_t>(annotationsAdded + addedCount);
  annotationsRemoved = static_cast<uint16_t>(annotationsRemoved + removedCount);

  LOG_INF("BookOrbit", "Applied %u server annotation(s) (%u new), %u deletion(s) (%u removed) (free=%u maxAlloc=%u)",
          (unsigned)appliedIds.size(), (unsigned)addedCount, (unsigned)deletedIds.size(), (unsigned)removedCount,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  incomingAnnotations.clear();
  if (appliedIds.empty() && deletedIds.empty()) return true;
  // Rides the sync's session: a nested Session would replace the shared client and close the
  // connection the activity is keeping alive for the progress upload.
  if (BookOrbitSyncClient::ackAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), appliedIds, deletedIds) !=
      BookOrbitSyncClient::OK) {
    LOG_ERR("BookOrbit", "Acknowledgment failed (http=%d); the server offers these again next sync",
            BookOrbitSyncClient::lastHttpCode);
    return false;
  }
  outLanded = appliedIds.size() + deletedIds.size();
  return true;
}
