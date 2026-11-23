#pragma once

#include <string>
#include <vector>

#include "network/BookOrbitCatalogClient.h"

struct BookOrbitCatalogLink;

/**
 * A catalog download that restarts the reader before it starts, so its TLS session
 * gets a heap browsing has not fragmented: the file to fetch, and the book listing
 * to come back to once it ends. The listing's pages themselves stay in this cache.
 */
struct BookOrbitDownloadResume {
  int64_t bookId = 0;
  int64_t fileId = 0;
  std::string title;
  std::string path;  // SD destination, already resolved from the book's detail
  BookOrbitBookQuery listQuery;
  // The read-status filter the listing opened with, before the user changed it.
  std::string listDefaultReadStatus;
  std::string listTitle;
  int listPage = 1;  // the last page the listing had in memory
  bool booksFromFacet = false;
  // Rows counted from the start of the listing, not from the first row in memory.
  int selectorIndex = 0;
  int topIndex = 0;
  // The facet listing Back returns to when booksFromFacet is set, and its row.
  std::string facetSectionId;
  std::string facetTitle;
  int facetPage = 1;
  int facetSelectorIndex = 0;
  int facetTopIndex = 0;
  // Started from a BookOrbit Home shelf: the download ends on Home, not on a listing.
  bool fromHome = false;
};

namespace BookOrbitCatalogListCache {

void clear();

bool loadRootSections(std::vector<BookOrbitCatalogSection>& outSections);
void saveRootSections(const std::vector<BookOrbitCatalogSection>& sections);

bool loadCatalogCounts(BookOrbitCatalogCounts& outCounts);
void saveCatalogCounts(const BookOrbitCatalogCounts& counts);

bool loadFacetPage(const std::string& sectionId, int page, BookOrbitFacetPage& outPage);
void saveFacetPage(const std::string& sectionId, int page, const BookOrbitFacetPage& pageData);

bool loadBooksPage(const BookOrbitBookQuery& query, int page, BookOrbitBookPage& outPage);
void saveBooksPage(const BookOrbitBookQuery& query, int page, const BookOrbitBookPage& pageData);

// Kept outside the listing pages, which clear() removes.
bool saveDownloadResume(const BookOrbitDownloadResume& resume);
// Reads the saved download and deletes the file whatever it held, so a crash
// during the download cannot replay it on every boot.
bool takeDownloadResume(BookOrbitDownloadResume& outResume);
void discardDownloadResume();

// The BookOrbit Home's link into the catalog, kept outside the listing pages too: on
// the X3 and X4 it waits here through the catalog's own network boot.
bool saveEntryLink(const BookOrbitCatalogLink& link);
// Reads the saved link and deletes the file; false, without logging, when none is saved.
bool takeEntryLink(BookOrbitCatalogLink& outLink);
void discardEntryLink();

}  // namespace BookOrbitCatalogListCache