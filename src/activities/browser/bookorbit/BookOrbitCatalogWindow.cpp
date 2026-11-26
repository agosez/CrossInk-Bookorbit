#include "BookOrbitCatalogWindow.h"

#include <algorithm>
#include <utility>

namespace {
using Entry = BookOrbitCatalogEntry;

// Reserved once for the session, so the rows never move through doubling reallocations
// that would leave holes next to the TLS buffers.
constexpr size_t WINDOW_ROWS = (BookOrbitCatalogWindow::MAX_PAGES + 1) * BookOrbitCatalogClient::PAGE_SIZE;
}  // namespace

void BookOrbitCatalogWindow::reset(const size_t rows, const int firstRow) {
  const size_t wanted = std::max(rows, WINDOW_ROWS);
  if (entries.capacity() > wanted) {
    // Free the large block first, so the smaller one can be carved out of it.
    std::vector<Entry>().swap(entries);
  }
  entries.clear();
  entries.reserve(wanted);
  first = firstRow;
  letterCount = 0;
}

void BookOrbitCatalogWindow::release() { std::vector<Entry>().swap(entries); }

const Entry* BookOrbitCatalogWindow::at(const int row) const {
  const int index = row - first;
  if (index < 0 || index >= static_cast<int>(entries.size())) return nullptr;
  return &entries[index];
}

Entry* BookOrbitCatalogWindow::at(const int row) { return const_cast<Entry*>(std::as_const(*this).at(row)); }

size_t BookOrbitCatalogWindow::placePage(const PageLoad load, const size_t count, const uint16_t page) {
  // An exact reserve: insert() would otherwise double the block. Only a listing whose
  // pages or screen are larger than WINDOW_ROWS planned for gets here.
  if (entries.size() + count > entries.capacity()) entries.reserve(entries.size() + count);
  Entry blank;
  blank.type = Entry::Type::BOOK;
  blank.page = page;
  if (load == PageLoad::Prepend) {
    entries.insert(entries.begin(), count, blank);
    first -= static_cast<int>(count);
    return 0;
  }
  const size_t firstIndex = entries.size();
  entries.insert(entries.end(), count, blank);
  return firstIndex;
}

void BookOrbitCatalogWindow::dropPagesBefore(const int keepFrom, const int incoming) {
  while (!entries.empty() && entries.back().page - entries.front().page + 1 + incoming > MAX_PAGES) {
    const uint16_t page = entries.front().page;
    const auto pageEnd =
        std::find_if(entries.begin(), entries.end(), [page](const Entry& entry) { return entry.page != page; });
    const int count = static_cast<int>(pageEnd - entries.begin());
    if (first + count > keepFrom) break;  // the viewport still shows that page
    entries.erase(entries.begin(), pageEnd);
    first += count;
  }
}

int BookOrbitCatalogWindow::dropPagesAfter(const int keepThrough, const int incoming) {
  int lastPage = 0;
  while (!entries.empty() && entries.back().page - entries.front().page + 1 + incoming > MAX_PAGES) {
    const uint16_t page = entries.back().page;
    const auto pageStart = std::find_if(entries.rbegin(), entries.rend(), [page](const Entry& entry) {
                             return entry.page != page;
                           }).base();
    if (first + static_cast<int>(pageStart - entries.begin()) <= keepThrough) break;
    entries.erase(pageStart, entries.end());
    lastPage = page - 1;
  }
  return lastPage;
}

int BookOrbitCatalogWindow::clamp(const int row) const {
  if (entries.empty()) return row;
  return std::clamp(row, first, end() - 1);
}

void BookOrbitCatalogWindow::setLetters(const std::vector<BookOrbitLetterBucket>& buckets) {
  letterCount = 0;
  for (const auto& bucket : buckets) {
    if (letterCount == MAX_LETTERS) break;
    LetterStart& slot = letters[letterCount++];
    slot.row = bucket.row;
    slot.heading[0] = bucket.letter;
  }
}

const char* BookOrbitCatalogWindow::heading(const size_t index) const {
  if (letterCount > 0) {
    const int row = first + static_cast<int>(index);
    for (size_t i = 0; i < letterCount; i++) {
      if (letters[i].row == row) return letters[i].heading;
    }
    return nullptr;
  }
  const Entry& entry = entries[index];
  if (entry.type != Entry::Type::BOOK || entry.sectionId.empty()) return nullptr;
  // The first row in memory may go on with a section that began on a page no longer held:
  // it is headed again, which names the section the screen opens on.
  if (index > 0 && entries[index - 1].sectionId == entry.sectionId) return nullptr;
  return entry.sectionId.c_str();
}
