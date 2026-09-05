#pragma once

#include <cstdint>
#include <string>

/**
 * Remembers where BookOrbit catalog downloads landed on the SD card, keyed by the
 * server's book id, so the catalog can mark books as already on the device even
 * when they were downloaded into a configurable folder or under an older folder
 * setting. Entries are verified against the filesystem on lookup (the file must
 * still exist with its recorded size) and dropped when stale, falling back to the
 * catalog's filename heuristic — this is a best-effort convenience cache, never a
 * source of truth.
 *
 * Backed by /.crosspoint/bookorbit_downloads.bin (see docs/file-formats.md). The
 * index is loaded lazily on first use and freed with unload(); the catalog
 * activity is its only user and runs on the main task, so access is not locked.
 * Book ids are scoped to one server: the file records the configured server URL
 * and is discarded when it no longer matches.
 */
namespace BookOrbitDownloadIndex {

// Fetches the recorded on-device path for a catalog book. Returns false when the
// book was never recorded or its file is gone or replaced (the stale entry is
// dropped and the index file updated).
bool lookup(int64_t bookId, std::string& outPath);

// Records where a catalog download landed. The file's current size is stored for
// lookup()'s replacement check; call only after the download fully succeeded.
void record(int64_t bookId, const std::string& path);

// Frees the in-RAM copy; the next lookup or record reloads it from the SD card.
void unload();

}  // namespace BookOrbitDownloadIndex
