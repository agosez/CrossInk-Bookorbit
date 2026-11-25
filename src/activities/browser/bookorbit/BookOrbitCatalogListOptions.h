#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "network/BookOrbitCatalogClient.h"

class OptionPopup;

/**
 * The two pickers of a BookOrbit server book list: the Library's sort methods that
 * BookOrbit can apply, in the Library's order, and BookOrbit's read-status filter. The
 * server does both, so paging stays exact. Sort and Left on button-only devices, Filter
 * and Right; a header control each on touch devices.
 */
namespace BookOrbitCatalogListOptions {

enum class Picker : uint8_t { Sort, Filter };

// What a pick asks of the listing.
enum class Change : uint8_t {
  None,      // nothing: dismissed, or the order or filter already in place
  ListView,  // the sort picker's list view row: the Library's sections on or off
  Listing,   // another order or filter: the listing loads again for the new query
};

// The order the query lists in: its own, or the sort's natural direction.
bool sortsDescending(const BookOrbitBookQuery& query);

// The label of a read-status filter, for the header.
const char* readStatusLabel(const std::string& readStatus);

// Fills the picker for the listing's query. onPick receives the row picked; applying it
// is left to the caller, once the picker has closed.
void show(OptionPopup& popup, Picker picker, const BookOrbitBookQuery& query, std::function<void(int)> onPick);

// The query the row picked asks for, in `next`, when the pick is Change::Listing.
Change apply(Picker picker, int choice, const BookOrbitBookQuery& current, BookOrbitBookQuery& next);

}  // namespace BookOrbitCatalogListOptions
