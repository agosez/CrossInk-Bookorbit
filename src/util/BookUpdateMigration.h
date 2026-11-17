#pragma once

#include <string>

/**
 * Carries a book's local state across the replacement of its file by a different copy, as a
 * BookOrbit catalog re-download does after the server rewrote the book.
 *
 * Highlights, bookmarks and the book's BookOrbit state (book_<hash>/: highlight positions,
 * sync marker, queued stats, reading progress) are named by the file's content hash
 * (BookContentId), which the new copy changes. Left alone they would stay under the old hash,
 * and the book would open with none of them.
 *
 * A highlight keeps its position when the chapter it points into is byte-identical in both
 * copies: the same spine slot holds the same ZIP entry with the same CRC-32. A highlight in any
 * other chapter is marked orphaned (ClippingStore::markOrphanedChapters): still listed, never
 * drawn, jumped to or synced. When the copies cannot be compared, every highlight is.
 *
 * Bookmarks and reading progress move as they are: a position in a changed chapter lands near
 * where it was, which is all either promises.
 *
 * Defined only with CROSSINK_APP_CAP_BOOK_UPDATE.
 */
namespace BookUpdateMigration {

/**
 * newPath holds the new copy; oldCopyPath the copy it replaced, still readable. Call after the
 * book's render cache was cleared, so the new copy's spine is read from the new file. Builds the
 * new copy's metadata cache on the way. Leaves oldCopyPath for the caller to remove.
 */
void apply(const std::string& newPath, const std::string& oldCopyPath);

}  // namespace BookUpdateMigration
