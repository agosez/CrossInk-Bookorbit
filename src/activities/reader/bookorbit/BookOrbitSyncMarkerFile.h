#pragma once

#include <cstdint>
#include <string>

#include "util/BookOrbitSmartSync.h"

/**
 * Smart sync's memory: what this device saw at the end of its last successful sync of a
 * book, one 18-byte file beside the book's stats queue. It holds the server's progress
 * timestamp and the position both sides were left at, which together answer the one
 * question the automatic decision needs -- has the server moved since we were last
 * here? -- and that progress percentages alone cannot. No readable marker means no
 * history, and no history always falls back to the choice screen.
 */
namespace BookOrbitSyncMarkerFile {

// bookStateDir is BookContentId::bookStateDir(): empty when the book is not hashable.
BookOrbitSmartSync::Marker read(const std::string& bookStateDir);

void write(const std::string& bookStateDir, int64_t timestamp, const BookOrbitSmartSync::Position& position);

}  // namespace BookOrbitSyncMarkerFile
