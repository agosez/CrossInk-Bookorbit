#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "BookOrbitCatalogRows.h"

/**
 * The rows the BookOrbit catalog holds in memory for the listing on screen: a whole root
 * or In progress list, or a window of a few consecutive pages of a book or facet listing,
 * or of the On device list.
 *
 * Rows are numbered from the start of the listing, so a page dropped from the window
 * changes no row number: rows()[0] is row start(). A listing used to keep every page it
 * had shown, about 150 bytes a row plus its strings, until a C3 catalog session had no
 * block left for a 16 KB TLS record by the third page (issue #96). Two pages cover a
 * screen and the next; a third arrives before the first one goes.
 *
 * The render task draws these rows: the browser changes them only under its RenderLock.
 */
class BookOrbitCatalogWindow {
 public:
  using Entry = BookOrbitCatalogEntry;

  // How a page joins the rows. Open starts a listing at that page; Append and Prepend
  // extend the window by one page at either end.
  enum class PageLoad : uint8_t { Open, Append, Prepend };

  // Server pages a book or facet listing keeps in memory.
  static constexpr int MAX_PAGES = 2;

  // Empties the rows for a new listing that will hold about `rows`, its first row in
  // memory numbered `firstRow`, and forgets its letters. Gives back a larger block a
  // previous In progress listing left behind.
  void reset(size_t rows, int firstRow = 0);
  // Frees the rows and their block: a download needs every KB of contiguous heap.
  void release();

  bool empty() const { return entries.empty(); }
  int start() const { return first; }
  int end() const { return first + static_cast<int>(entries.size()); }
  // The row at a listing row number, nullptr when it is not in memory.
  const Entry* at(int row) const;
  Entry* at(int row);
  // The rows in memory: a whole listing's builder (the root, In progress) fills them
  // straight after reset().
  std::vector<Entry>& rows() { return entries; }
  const std::vector<Entry>& rows() const { return entries; }
  // The page the first row in memory came from; 0 when the window is empty or whole.
  uint16_t firstPage() const { return entries.empty() ? 0 : entries.front().page; }

  // Room for `count` blank rows of page `page`, at the front for Prepend and at the end
  // otherwise; returns the index of the first.
  size_t placePage(PageLoad load, size_t count, uint16_t page);
  // Drop whole pages from the front while the window holds more than MAX_PAGES (counting
  // `incoming` pages about to join it) and the page lies wholly before row keepFrom.
  void dropPagesBefore(int keepFrom, int incoming);
  // The same from the end, for pages wholly after row keepThrough. Returns the last page
  // the window then holds when it dropped any, 0 when it dropped none.
  int dropPagesAfter(int keepThrough, int incoming);
  // The row in memory nearest `row`; `row` itself when there is none.
  int clamp(int row) const;

  // Where each letter starts in a listing sorted by title or author, from its first page.
  void setLetters(const std::vector<BookOrbitLetterBucket>& letters);
  // The section heading drawn above rows()[index], or nullptr: the letter that starts
  // there, or for a book list without letters the section the sort files the row under.
  const char* heading(size_t index) const;

 private:
  // Where each letter starts in a listing sorted by title or author; none for any other
  // listing. The heading strings stay put for the list to point at.
  struct LetterStart {
    int row = 0;
    char heading[2] = {'#', '\0'};
  };
  static constexpr size_t MAX_LETTERS = 27;  // A to Z, and '#'

  std::vector<Entry> entries;
  int first = 0;
  std::array<LetterStart, MAX_LETTERS> letters{};
  size_t letterCount = 0;
};
