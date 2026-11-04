#pragma once

#include <cstdint>
#include <string>

/** Where the BookOrbit Home enters the catalog: one book to download, or a drill-down section. */
struct BookOrbitCatalogLink {
  int64_t bookId = 0;
  std::string sectionId;
  std::string title;
};
