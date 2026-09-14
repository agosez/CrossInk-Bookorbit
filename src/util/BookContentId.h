#pragma once

#include <string>

/**
 * Content-based book identity, shared by every store that must survive a book being
 * moved or renamed on the SD card.
 *
 * The identity is the partial-MD5 content hash BookOrbit sync already reports to the
 * server (KOReaderDocumentId), so local state and server state agree on what "the same
 * book" means: a moved file keeps its identity, a different edition of the same title is
 * a different book, and two byte-identical copies share one state.
 */
namespace BookContentId {

/**
 * 32-hex content hash of the book file, or an empty string when the file cannot be read.
 *
 * Memoized for the most recently hashed file, so repeated lookups while a book is open
 * (clippings, bookmarks, BookOrbit stores) cost no SD reads after the first. Safe to call
 * from the render task: the reader's onEnter clipping load warms the memo on the main
 * task first, and a cold call only reads the book file the way that task already does.
 */
std::string contentHash(const std::string& filePath);

/**
 * Name of the directory holding a book's content-keyed state
 * ("/.crosspoint/book_<hash>"), with no side effects: nothing is created, nothing is
 * migrated. For read paths — home screens and boot resume consult it for every listed
 * book, and must not litter the card with empty directories.
 *
 * Returns an empty string when the book cannot be hashed.
 */
std::string bookStateDirName(const std::string& filePath);

/**
 * bookStateDirName() plus writer-side setup: the directory is created on first use, and
 * the first call per book each boot also moves any bookorbit_* files out of the book's
 * path-keyed cache directory, so state written by earlier releases (or preserved through
 * a cache clear) is not orphaned. The directory also holds the authoritative copy of the
 * book's reading progress (see EpubReaderUtils::saveProgress).
 *
 * Returns an empty string when the book cannot be hashed; the BookOrbit stores treat
 * that as a failed operation rather than touching a wrong path.
 */
std::string bookStateDir(const std::string& filePath);

}  // namespace BookContentId
