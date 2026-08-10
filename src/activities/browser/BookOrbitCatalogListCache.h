#pragma once

#include <string>
#include <vector>

#include "network/BookOrbitCatalogClient.h"

namespace BookOrbitCatalogListCache {

void clear();

bool loadRootSections(std::vector<BookOrbitCatalogSection>& outSections);
void saveRootSections(const std::vector<BookOrbitCatalogSection>& sections);

bool loadFacetPage(const std::string& sectionId, int page, BookOrbitFacetPage& outPage);
void saveFacetPage(const std::string& sectionId, int page, const BookOrbitFacetPage& pageData);

bool loadBooksPage(const BookOrbitBookQuery& query, int page, BookOrbitBookPage& outPage);
void saveBooksPage(const BookOrbitBookQuery& query, int page, const BookOrbitBookPage& pageData);

}  // namespace BookOrbitCatalogListCache