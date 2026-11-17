#pragma once

#include <cstddef>
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
 * index is streamed from the SD card. Catalog access and book moves through
 * relocate() all run on the main task, so access is not locked.
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

// Visits every recorded download whose file is still there, unchanged. This is what
// lets the catalog list its own downloads wherever the server's file naming template
// filed them, without walking the card.
// An optional id filter skips filesystem checks for books outside a catalog page.
void forEachExisting(void (*visit)(int64_t bookId, const std::string& path, void* context), void* context,
                     bool (*include)(int64_t bookId, void* context) = nullptr);

// Follows a recorded download that was renamed or moved on the device, so the catalog
// still finds it. No-op when oldPath was never a catalog download.
void relocate(const std::string& oldPath, const std::string& newPath);

// Downloads recorded for the configured server, without checking their files: a book
// deleted outside the catalog stays counted until a lookup finds it gone.
size_t recordedCount();

}  // namespace BookOrbitDownloadIndex
