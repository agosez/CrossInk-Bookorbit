#pragma once

#include <string>
#include <vector>

#include "network/BookOrbitCatalogClient.h"

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
  std::string listTitle;
  int listPage = 1;
  bool booksFromFacet = false;
  int selectorIndex = 0;
  int topIndex = 0;
  // The facet listing Back returns to when booksFromFacet is set.
  std::string facetSectionId;
  std::string facetTitle;
  int facetPage = 1;
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

}  // namespace BookOrbitCatalogListCache