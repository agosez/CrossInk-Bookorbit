#pragma once

#include <BookOrbitHomeShelfSource.h>
#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>

// Text the BookOrbit Home's pages share, cut from translated strings or formatted into
// caller buffers.
namespace BookOrbitHomeText {

// Copies the index-th character of a translated initials string: "LMMJVSD" holds one
// character per weekday, and some languages need more than one byte for them.
void copyInitial(const char* initials, int index, char* out, size_t outSize);

// Copies the index-th comma-separated token of a translated list.
void copyToken(const char* list, int index, char* out, size_t outSize);

void formatHours(uint32_t hours, char* out, size_t outSize);

// The title of Discover, Recently Added or Want to Read, the kinds the server does not name.
StrId shelfKindName(BookOrbitHomeShelfKind kind);

// A Library page shelf's title: a collection or SmartScope goes by its own name.
const char* shelfTitle(const BookOrbitHomeShelfSource& source);

}  // namespace BookOrbitHomeText
