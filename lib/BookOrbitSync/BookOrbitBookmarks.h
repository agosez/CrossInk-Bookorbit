#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "BookOrbitAnnotations.h"  // key rows and datetime helpers are shared constructions

/**
 * Wire types for BookOrbit's two-way bookmark exchange.
 *
 * Bookmarks are position-only: no text, no style -- which is also why the server cannot run
 * the read-back text verification it applies to annotations. Identity is md5(datetime | pos),
 * built with the same helpers as annotations.
 *
 * The identity convention INVERTS the annotation one: for a bookmark created on the web, the
 * DEVICE mints the datetime when it applies the entry, and reports {key, datetime, pos} in the
 * acknowledgment -- without that triplet the server has nothing to link its copy to.
 */

// A bookmark on its way to the server.
struct BookOrbitBookmark {
  char datetime[20] = {};  // "YYYY-MM-DD HH:MM:SS"
  std::string pos;         // xpointer of the bookmarked page's first codepoint
  std::string chapter;
  uint16_t pageno = 0;
};

// A change the server wants applied: a bookmark created on the web, or a deletion.
struct BookOrbitIncomingBookmark {
  uint32_t serverId = 0;
  // A deletion names its target by `key`, the md5 this device reported; an add carries `pos`.
  std::string key;
  std::string pos;
  std::string title;  // the server's label for the bookmark; used as the chapter fallback
  uint16_t pageno = 0;
  bool deleted = false;
};

/**
 * One acknowledgment line. For an applied add, the local identity triplet must be filled: it
 * is what links the server's bookmark to this device. `failed` parks an entry the device can
 * never place (its position does not parse); an entry that might resolve later is simply left
 * out of the acknowledgment and offered again.
 */
struct BookOrbitBookmarkAck {
  uint32_t serverId = 0;
  bool failed = false;
  char key[BookOrbitAnnotationKey::DIGEST_SIZE] = {};
  char datetime[20] = {};
  std::string pos;
};

// Bookmarks carry no text, so entries are small; the byte budget still bounds the body.
inline constexpr size_t BOOKORBIT_BOOKMARK_BATCH = 16;
