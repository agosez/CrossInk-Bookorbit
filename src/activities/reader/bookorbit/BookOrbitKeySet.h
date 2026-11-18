#pragma once

#include <vector>

#include "BookOrbitAnnotationStore.h"
#include "BookOrbitAnnotations.h"
#include "BookOrbitBookmarkStore.h"

/**
 * The key sets the highlight and bookmark exchanges send: one row per record of the book's
 * sync-state store, hashed from its datetime and position as the server hashes them. Comparing
 * its own set against this one is how the server notices what was deleted here.
 *
 * Both are built in this one file so the vector code behind them is linked once: built in two
 * files, GCC inlined a copy of it into each.
 */
namespace BookOrbitKeySet {

// False, with keys left empty, when a row cannot be built: the set then goes out as incomplete.
bool build(const std::vector<BookOrbitAnnotationRecord>& records, std::vector<BookOrbitAnnotationKey>& keys);
bool build(const std::vector<BookOrbitBookmarkRecord>& records, std::vector<BookOrbitAnnotationKey>& keys);

}  // namespace BookOrbitKeySet
