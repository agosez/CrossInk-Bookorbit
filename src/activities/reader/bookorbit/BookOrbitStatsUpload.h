#pragma once

#include <cstddef>
#include <string>

class BookOrbitSyncReporter;

namespace BookOrbitStatsUpload {

/**
 * Uploads the reading-session events the reader queued for this book (see
 * BookOrbitStatsQueue) on the sync's open session, one batch per request, and clears the
 * queue once all of it is accepted. Upload-only: BookOrbit has no stats download API, so
 * stats flow CrossInk -> BookOrbit.
 *
 * Returns how many events the server accepted this sync, reported in the sweep record
 * that closes the sync (see BookOrbitSyncClient::completeSweep).
 */
size_t uploadQueued(const std::string& epubPath, const std::string& documentHash, BookOrbitSyncReporter& reporter);

}  // namespace BookOrbitStatsUpload
