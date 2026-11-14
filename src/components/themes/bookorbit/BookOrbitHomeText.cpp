#include "BookOrbitHomeText.h"

#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace BookOrbitHomeText {

void copyInitial(const char* initials, const int index, char* out, const size_t outSize) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(initials);
  for (int i = 0; i < index; i++) utf8NextCodepoint(&cursor);
  const auto* start = cursor;
  utf8NextCodepoint(&cursor);
  const size_t length = std::min(static_cast<size_t>(cursor - start), outSize - 1);
  memcpy(out, start, length);
  out[length] = '\0';
}

void copyToken(const char* list, int index, char* out, const size_t outSize) {
  while (index > 0 && *list != '\0') {
    if (*list++ == ',') index--;
  }
  size_t length = 0;
  while (list[length] != '\0' && list[length] != ',' && length + 1 < outSize) {
    out[length] = list[length];
    length++;
  }
  out[length] = '\0';
}

void formatHours(const uint32_t hours, char* out, const size_t outSize) {
  snprintf(out, outSize, "%luh", static_cast<unsigned long>(hours));
}

StrId shelfKindName(const BookOrbitHomeShelfKind kind) {
  if (kind == BookOrbitHomeShelfKind::RECENTLY_ADDED) return StrId::STR_HOME_RECENTLY_ADDED;
  if (kind == BookOrbitHomeShelfKind::WANT_TO_READ) return StrId::STR_BOOKORBIT_WANT_TO_READ;
  return StrId::STR_HOME_DISCOVER;
}

const char* shelfTitle(const BookOrbitHomeShelfSource& source) {
  // The server names every collection and SmartScope; its id stands in for a name lost.
  if (isNamedHomeShelfKind(source.kind)) return source.name[0] != '\0' ? source.name : source.id;
  return I18N.get(shelfKindName(source.kind));
}

}  // namespace BookOrbitHomeText
