#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "network/BookOrbitCatalogClient.h"

/** One row of the BookOrbit catalog's listings. */
struct BookOrbitCatalogEntry {
  enum class Type { SECTION, FACET_SECTION, FACET, LOCAL_SECTION, LOCAL_BOOK, SEARCH, BOOK };

  Type type;
  std::string title;
  std::string subtitle;  // author, for BOOK/LOCAL_BOOK entries
  // Section id (SECTION/FACET_SECTION/LOCAL_SECTION), facet entry id (FACET), or for a
  // BOOK the list section its listing's sort files it under (its series, the day it was
  // added), empty when that sort has none.
  std::string sectionId;
  std::string seriesId;  // numeric series id, for FACET entries of the series facet
  std::string path;      // SD path: LOCAL_BOOK entries, and BOOK entries already on the device
  int64_t bookId = 0;
  bool onDevice = false;  // BOOK entries: a matching file already exists on the device (at `path`)
  uint16_t page = 0;      // listing page the row came from; 0 for the root and In progress
  // Length of `title` before its " (count)" badge, 0 when it has none. A listing opened
  // from the row is titled without it: its header counts what the listing holds.
  uint16_t badgeAt = 0;
};

/**
 * What each catalog listing's rows hold, made from what the server or the SD card
 * returned: the root's sections with their counts, a facet listing's entries, a server
 * book list's books with their on-device mark, and the two on-device lists. The browser
 * decides where the rows go (see BookOrbitCatalogWindow); these say what is in them.
 */
namespace BookOrbitCatalogRows {

// Root sections that list facets (authors, series...) rather than books.
bool isFacetSection(const std::string& sectionId);

// A row's title without the count badge the root and facet listings give it.
std::string titleWithoutBadge(const BookOrbitCatalogEntry& entry);

// Rows the root holds beyond the server's sections: Want to read, the two on-device lists
// and Search.
constexpr size_t ROOT_EXTRA_ROWS = 4;

// The root: the server's sections, badged with the counts the server gave (an older
// server without the dashboard gives none), the want-to-read shelf when the server has
// one, the On device and In progress lists with their counts, then Search.
void appendRoot(std::vector<BookOrbitCatalogEntry>& rows, const std::vector<BookOrbitCatalogSection>& sections,
                const BookOrbitCatalogCounts& counts, int onDeviceCount, size_t inProgressCount);

// The books In progress lists: those with local reading progress, the recent-books list
// minus finished ones. Appends their rows when a sink is given, and returns the count either
// way, so the root can badge the category without building its rows.
size_t collectInProgress(std::vector<BookOrbitCatalogEntry>* sink);

// In progress in title order, then author.
void sortByTitle(std::vector<BookOrbitCatalogEntry>& rows);

// The rows of a facet page, a server book page and an On device page, each filled in
// place (see BookOrbitCatalogWindow::placePage) and taking the strings it is given.
void setFacet(BookOrbitCatalogEntry& entry, BookOrbitFacetEntry&& facet);
void setBook(BookOrbitCatalogEntry& entry, BookOrbitCatalogBook&& book);
void setLocalBook(BookOrbitCatalogEntry& entry, std::string&& path);

// The book listing a facet row opens; facetSectionId is the facet listing's own section.
BookOrbitBookQuery facetBookQuery(const std::string& facetSectionId, const BookOrbitCatalogEntry& facet);

}  // namespace BookOrbitCatalogRows
