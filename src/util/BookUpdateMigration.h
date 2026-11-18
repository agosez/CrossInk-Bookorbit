#pragma once

#include <string>

class Epub;

/**
 * Carries a book's local state across the replacement of its file by a different copy, as a
 * BookOrbit catalog re-download does after the server rewrote the book.
 *
 * Highlights, bookmarks and the book's BookOrbit state (book_<hash>/: highlight positions,
 * sync marker, queued stats, reading progress) are named by the file's content hash
 * (BookContentId), which the new copy changes. Left alone they would stay under the old hash,
 * and the book would open with none of them.
 *
 * Two steps, so neither needs more than a cached book's open:
 * - begin(), once the new copy is in place and the old one waits at "<book>.old"
 *   (HttpDownloader's keepReplaced): moves the stores to the new hash, notes which ZIP entry
 *   (path and CRC-32) each chapter of the old copy read, rewinds the upload watermarks so the
 *   server gets every highlight and bookmark again, clears the render cache and removes the
 *   old copy. The old spine comes from the render cache built for the old copy, so nothing is
 *   parsed. A reset before it ends leaves "<book>.old", and the next open of the book, or the
 *   next download to its path, runs it again.
 * - finish(), when the reader opens the new copy: a highlight keeps its place when its spine
 *   slot reads the same entry with the same CRC-32 in both copies; any other is marked orphaned
 *   (ClippingStore::markOrphanedChapters): still listed, never drawn, jumped to or synced.
 *
 * Bookmarks and reading progress move as they are: a position in a changed chapter lands near
 * where it was, which is all either promises.
 */
namespace BookUpdateMigration {

// A download over bookPath left the copy it replaced beside it, still to be carried across.
bool pending(const std::string& bookPath);

// See above. Call before anything opens the new copy's render cache. renderCacheIsOld: the
// download that replaced the copy has just ended, so the book's render cache still describes
// the old copy; false when resuming, where only the old file can be trusted.
void begin(const std::string& bookPath, bool renderCacheIsOld);

// See above. epub is the new copy, loaded. A single exists() for a book with nothing waiting.
void finish(const Epub& epub);

}  // namespace BookUpdateMigration
