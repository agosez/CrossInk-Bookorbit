#include "BookOrbitCatalogRows.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <utility>

#include "BookOrbitCatalogDownload.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/HeaderDate.h"

namespace BookOrbitCatalogRows {
namespace {
using Entry = BookOrbitCatalogEntry;

// Appends a row's " (count)" badge, remembering where the title itself ends.
void addCountBadge(Entry& entry, const int count) {
  entry.badgeAt = static_cast<uint16_t>(entry.title.size());
  entry.title += " (" + std::to_string(count) + ")";
}

// The root section's entry count, the way BookOrbit's own plugin badges its Browse tiles:
// -1 for a section it has none for.
int sectionCount(const BookOrbitCatalogCounts& counts, const std::string& id) {
  if (id == "all-books" || id == "recent") return counts.totalBooks;
  if (id == "continue-reading") return counts.inProgress;
  if (id == "libraries") return counts.libraries;
  if (id == "authors") return counts.authors;
  if (id == "series") return counts.series;
  if (id == "collections") return counts.collections;
  if (id == "smart-scopes") return counts.smartScopes;
  return -1;
}

std::string bookTitleFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t end = path.size() > start + 5 ? path.size() - 5 : path.size();  // strip ".epub"
  return path.substr(start, end - start);
}
}  // namespace

bool isFacetSection(const std::string& sectionId) {
  return sectionId == "authors" || sectionId == "series" || sectionId == "collections" || sectionId == "libraries" ||
         sectionId == "smart-scopes";
}

std::string titleWithoutBadge(const Entry& entry) {
  return entry.badgeAt > 0 ? entry.title.substr(0, entry.badgeAt) : entry.title;
}

void appendRoot(std::vector<Entry>& rows, const std::vector<BookOrbitCatalogSection>& sections,
                const BookOrbitCatalogCounts& counts, const int onDeviceCount, const size_t inProgressCount) {
  for (const auto& section : sections) {
    Entry entry;
    entry.type = isFacetSection(section.id) ? Entry::Type::FACET_SECTION : Entry::Type::SECTION;
    entry.title = section.title;
    const int count = sectionCount(counts, section.id);
    if (count >= 0) addCountBadge(entry, count);
    entry.sectionId = section.id;
    rows.push_back(std::move(entry));
  }
  // The want-to-read shelf BookOrbit's own plugin shows, which the server does not list
  // among its sections. After them, so the rows above keep their places.
  if (counts.wantToRead >= 0) {
    Entry wantToRead;
    wantToRead.type = Entry::Type::SECTION;
    wantToRead.title = tr(STR_BOOKORBIT_WANT_TO_READ);
    addCountBadge(wantToRead, counts.wantToRead);
    wantToRead.sectionId = BookOrbitCatalogClient::WANT_TO_READ_SHELF;
    rows.push_back(std::move(wantToRead));
  }
  Entry onDevice;
  onDevice.type = Entry::Type::LOCAL_SECTION;
  onDevice.title = tr(STR_BOOKORBIT_ON_DEVICE);
  addCountBadge(onDevice, onDeviceCount);
  onDevice.sectionId = "on-device";
  rows.push_back(std::move(onDevice));
  Entry inProgress;
  inProgress.type = Entry::Type::LOCAL_SECTION;
  inProgress.title = tr(STR_BOOKORBIT_IN_PROGRESS);
  addCountBadge(inProgress, static_cast<int>(inProgressCount));
  inProgress.sectionId = "in-progress";
  rows.push_back(std::move(inProgress));
  Entry search;
  search.type = Entry::Type::SEARCH;
  search.title = tr(STR_SEARCH);
  rows.push_back(std::move(search));
}

size_t collectInProgress(std::vector<Entry>* sink) {
  size_t count = 0;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!FsHelpers::hasEpubExtension(book.path) || !Storage.exists(book.path.c_str())) continue;
    const BookReadingStats stats = BookReadingStats::load(Epub::cachePathForFilePath(book.path, "/.crosspoint"));
    if (stats.isCompleted) continue;
    count++;
    if (!sink) continue;
    Entry entry;
    entry.type = Entry::Type::LOCAL_BOOK;
    entry.title = book.title.empty() ? book.path : book.title;
    entry.subtitle = book.author;
    entry.path = book.path;
    sink->push_back(std::move(entry));
  }
  LOG_DBG("BookOrbit", "Local scan: kind=in-progress count=%u", static_cast<unsigned>(count));
  return count;
}

void sortByTitle(std::vector<Entry>& rows) {
  std::sort(rows.begin(), rows.end(), [](const Entry& a, const Entry& b) {
    if (FsHelpers::naturalLess(a.title, b.title)) return true;
    if (FsHelpers::naturalLess(b.title, a.title)) return false;
    return FsHelpers::naturalLess(a.subtitle, b.subtitle);
  });
}

void setFacet(Entry& entry, BookOrbitFacetEntry&& facet) {
  entry.type = Entry::Type::FACET;
  entry.title = std::move(facet.title);
  if (facet.count > 0) addCountBadge(entry, facet.count);
  entry.sectionId = std::move(facet.id);
  entry.seriesId = std::move(facet.seriesId);
}

void setBook(Entry& entry, BookOrbitCatalogBook&& book) {
  entry.type = Entry::Type::BOOK;
  entry.bookId = book.id;
  entry.onDevice = BookOrbitCatalogDownload::findOnDevice(book.id, book.title, book.author, entry.path);
  entry.title = std::move(book.title);
  entry.subtitle = std::move(book.author);
  // The list section the sort files the book under. The day is formatted once, here:
  // short enough to fit the string without a heap block.
  if (!book.series.empty()) {
    entry.sectionId = std::move(book.series);
  } else if (book.addedAt != 0) {
    char day[24];
    if (formatLocalDateText(day, sizeof(day), book.addedAt)) entry.sectionId = day;
  }
}

void setLocalBook(Entry& entry, std::string&& path) {
  entry.type = Entry::Type::LOCAL_BOOK;
  entry.title = bookTitleFromPath(path);
  entry.path = std::move(path);
}

BookOrbitBookQuery facetBookQuery(const std::string& facetSectionId, const Entry& facet) {
  // Mirror BookOrbit's own plugin (paramsForEntry): author filters by the
  // entry id; series prefers the numeric seriesId and sorts by series order;
  // collections, libraries and SmartScopes filter by their numeric id and
  // sort by title (the server's own booksHref for all three).
  BookOrbitBookQuery query;
  if (facetSectionId == "series") {
    query.sort = "series";
    if (!facet.seriesId.empty()) {
      query.seriesId = facet.seriesId;
    } else {
      query.series = facet.sectionId;
    }
  } else if (facetSectionId == "collections") {
    query.sort = "title";
    query.collectionId = facet.sectionId;
  } else if (facetSectionId == "libraries") {
    query.sort = "title";
    query.libraryId = facet.sectionId;
  } else if (facetSectionId == "smart-scopes") {
    query.sort = "title";
    query.smartScopeId = facet.sectionId;
  } else {
    query.author = facet.sectionId;
  }
  return query;
}

}  // namespace BookOrbitCatalogRows
