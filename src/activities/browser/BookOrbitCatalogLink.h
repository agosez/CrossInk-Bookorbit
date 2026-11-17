#pragma once

#include <cstdint>
#include <string>

/**
 * Where the catalog is entered from outside it: one book to download, or a root section to open.
 * The BookOrbit Home sends the first two; a sync that finds the open book's copy out of date
 * sends a book with `replacePath`, the copy to download over, and the reader reopens it after.
 */
struct BookOrbitCatalogLink {
  int64_t bookId = 0;
  std::string sectionId;
  std::string title;
  std::string replacePath;
};
