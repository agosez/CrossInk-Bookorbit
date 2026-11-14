#pragma once

#include <cstdint>
#include <cstring>

// What a shelf of the BookOrbit Home's Library page lists. Saved as a number in
// bookorbit.json and on the SD card, so values are only ever appended.
enum class BookOrbitHomeShelfKind : uint8_t {
  DISCOVER = 0,        // the dashboard's random draw
  RECENTLY_ADDED = 1,  // the catalog's newest books
  WANT_TO_READ = 2,    // the dashboard's want-to-read shelf
  COLLECTION = 3,      // one collection, by id
  SMART_SCOPE = 4,     // one SmartScope, by id
};

constexpr int BOOKORBIT_HOME_SHELF_KIND_COUNT = 5;

// Collections and SmartScopes are the server's own lists, picked by id.
constexpr bool isNamedHomeShelfKind(const BookOrbitHomeShelfKind kind) {
  return kind == BookOrbitHomeShelfKind::COLLECTION || kind == BookOrbitHomeShelfKind::SMART_SCOPE;
}

// Where a shelf's books come from. The shelf settings, each entry of the list they are
// picked from (/.crosspoint/bookorbit_home_shelves.bin) and the head of a Home snapshot
// shelf all hold one, so a choice is a plain copy.
struct BookOrbitHomeShelfSource {
  BookOrbitHomeShelfKind kind = BookOrbitHomeShelfKind::DISCOVER;
  char id[20] = {};    // the collection's or SmartScope's id; empty for the other kinds
  char name[46] = {};  // its name as the server gave it, shown until the next refresh

  // The same books, whatever the name says.
  bool sameAs(const BookOrbitHomeShelfSource& other) const { return kind == other.kind && strcmp(id, other.id) == 0; }
};
