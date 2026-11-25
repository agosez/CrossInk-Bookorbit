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
 * Backed by /.crosspoint/bookorbit_downloads.bin (see docs/file-formats.md). Nothing of it
 * stays in RAM: each call reads the file's fixed-size records in one pass, a few at a time,
 * and reads a path back only for a record that matches. A question about many books (a
 * catalog page, Home's shelves, a batch of the device scan) is therefore asked once for all
 * of them: the single-book calls are batches of one, and a pass per book would read the
 * records again each time. Every caller runs on the main task, so access is not locked.
 * Book ids are scoped to one server: the file records the configured server URL and is
 * discarded when it no longer matches.
 */
namespace BookOrbitDownloadIndex {

// Visits each of the n books whose recorded file is still there, unchanged: found(i, path,
// context), with i the book's position in bookIds. One pass over the index; entries whose
// file is gone or was replaced are dropped, in one rewrite. found must not call back into
// the index.
void lookupAll(const int64_t* bookIds, size_t n, void (*found)(size_t i, const std::string& path, void* context),
               void* context);

// lookupAll() for one book: its recorded path, or false.
bool lookup(int64_t bookId, std::string& outPath);

struct Download {
  int64_t bookId;
  const std::string* path;
};

// Records where catalog downloads landed, in one rewrite. Each file's current size is stored
// for lookup()'s replacement check: call only once the download fully succeeded. One entry per
// book and one per path: a book recorded again replaces its old entry, and a book recorded onto
// another book's file supersedes that entry. Past the cap, the oldest entries go.
void recordAll(const Download* downloads, size_t n);

// recordAll() for one download.
void record(int64_t bookId, const std::string& path);

// Sets outClaimed[i] when some recorded download claims exactly paths[i]. One pass: lets a
// directory scan skip the files the index already holds.
void findPaths(const std::string* paths, size_t n, bool* outClaimed);

// findPaths() for one path.
bool hasPath(const std::string& path);

// Visits every recorded download whose file is still there, unchanged. This is what
// lets the catalog list its own downloads wherever the server's file naming template
// filed them, without walking the card. visit must not call back into the index.
void forEachExisting(void (*visit)(int64_t bookId, const std::string& path, void* context), void* context);

// The catalog book a recorded download at this exact path came from. Unlike lookup(), the
// file's size is not checked: the caller is about to replace that copy, typically because
// the server's file changed after it was downloaded.
bool bookIdForPath(const std::string& path, int64_t& outBookId);

// Follows a recorded download that was renamed or moved on the device, so the catalog
// still finds it. No-op when oldPath was never a catalog download.
void relocate(const std::string& oldPath, const std::string& newPath);

// Hash of a path as the index stores it (32-bit FNV-1a), for callers keeping their own
// record of paths beside it.
uint32_t pathHash(const std::string& path);

// Downloads recorded for the configured server, without checking their files: a book
// deleted outside the catalog stays counted until a lookup finds it gone.
size_t recordedCount();

}  // namespace BookOrbitDownloadIndex
