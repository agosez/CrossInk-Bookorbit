#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "network/HttpDownloader.h"

/**
 * A top-level BookOrbit catalog section (e.g. "Recently added", "Continue reading",
 * "All books", or the authors/series/collections/libraries/smart-scopes
 * drill-down facets).
 */
struct BookOrbitCatalogSection {
  std::string id;
  std::string title;
};

/** A single downloadable file attached to a BookOrbit book. */
struct BookOrbitCatalogFile {
  int64_t id = 0;
  std::string format;  // e.g. "epub", lowercase
  size_t sizeBytes = 0;
  // Relative path the server's KOReader file naming template resolves to for this
  // account and device. Empty on servers that predate the field; the caller then
  // names the file itself.
  std::string devicePath;
};

/** One entry in a BookOrbit book listing. */
struct BookOrbitCatalogBook {
  int64_t id = 0;
  std::string title;
  std::string author;  // first author only, for compact list display
  // What the listing's sort groups the book by, fetched only for the sort that needs it:
  // the series name under the series sort, and when it was added (UTC epoch seconds, 0
  // unknown) under the date-added sort.
  std::string series;
  uint32_t addedAt = 0;
};

/** Where a letter starts in a listing sorted by title or author, as the server ranks it. */
struct BookOrbitLetterBucket {
  char letter = '#';  // 'A' to 'Z' with accents folded, '#' for anything else
  int row = 0;        // the letter's first row, counted from the start of the listing
};

/** One entry in a drill-down facet listing (an author, a series or a collection). */
struct BookOrbitFacetEntry {
  std::string id;        // filter value for the books listing
  std::string title;     // display name
  std::string seriesId;  // numeric series id when the server provides one (series only)
  int count = 0;         // number of books, 0 when the server omits it
};

/** Result of a paged facet listing request (authors or series). */
struct BookOrbitFacetPage {
  std::vector<BookOrbitFacetEntry> entries;
  int page = 1;
  bool hasNext = false;
};

/** Filters for a book listing request; empty fields are omitted from the query. */
struct BookOrbitBookQuery {
  std::string sort;        // BookOrbit sort id (e.g. "recently_added", "title", "series")
  std::string order;       // "asc" or "desc"; empty = the sort's natural direction
  std::string readStatus;  // read-status filter: "unread", "reading" or "finished"
  // A dashboard shelf ("want-to-read") listed instead of the books endpoint: at most a
  // dozen books, in the server's order, which no other field applies to.
  std::string shelf;
  std::string query;         // free-text search
  std::string author;        // author filter (facet entry id)
  std::string seriesId;      // numeric series filter (preferred when present)
  std::string series;        // series-name filter (fallback when no seriesId)
  std::string collectionId;  // numeric collection filter (facet entry id)
  std::string smartScopeId;  // numeric SmartScope filter (facet entry id)
  std::string libraryId;     // numeric library filter; combinable with every field above
};

/** Full detail for a single BookOrbit book, including its downloadable files. */
struct BookOrbitBookDetail {
  int64_t id = 0;
  std::string title;
  std::string author;
  std::vector<BookOrbitCatalogFile> files;
};

/** Result of a paged book listing request. */
struct BookOrbitBookPage {
  std::vector<BookOrbitCatalogBook> books;
  int page = 1;
  int total = 0;
  int pageSize = 0;
  // The whole listing's letters, in its order: page 1 of a title or author sort only.
  std::vector<BookOrbitLetterBucket> letters;
};

/**
 * Per-section entry counts for the catalog root — the numbers BookOrbit's own
 * KOReader plugin shows on its Browse tiles. The book counts are the totals of
 * the EPUB-only listings the tiles open; the facet counts come from the server's
 * dashboard and cover every format, since the server cannot filter those.
 * -1 = unknown (older server without the dashboard, field absent, or failed fetch).
 */
struct BookOrbitCatalogCounts {
  int totalBooks = -1;  // "All Books" and "Recently added" list every EPUB
  int inProgress = -1;  // "Continue reading": EPUBs marked as being read
  int libraries = -1;
  int authors = -1;
  int series = -1;
  int collections = -1;
  int smartScopes = -1;
  // EPUBs on the dashboard's want-to-read shelf (the dozen most recently marked); -1 when
  // the server has no such shelf.
  int wantToRead = -1;
};

/**
 * HTTP client for BookOrbit's KOReader-authenticated JSON catalog endpoints
 * (browsing and downloading books). Uses the same x-auth-user/x-auth-key headers
 * as BookOrbitSyncClient; unlike the sync client, catalog requests go through
 * HttpDownloader since responses can be larger than a fixed-size buffer and file
 * downloads must stream straight to the SD card. Lives under src/network (rather
 * than lib/BookOrbitSync) because it depends on HttpDownloader, an app-level
 * (src/) utility.
 *
 * Only a simplified subset of BookOrbit's catalog is supported: the direct book
 * listings (recently added, continue reading, all books, search), the
 * authors/series/collections/libraries/smart-scopes drill-down facets, the
 * root's per-section counts, and downloading an EPUB file from a book's detail.
 * Covers, ratings and read-status editing are out of scope.
 */
class BookOrbitCatalogClient {
 public:
  static constexpr int PAGE_SIZE = 20;
  // The dashboard shelf of books marked "want to read", which BookOrbit's KOReader plugin
  // shows; the catalog lists it as a root section of the same id.
  static constexpr char WANT_TO_READ_SHELF[] = "want-to-read";

  // True when the last failed fetch reached a server but got a non-catalog reply
  // (HTML page, invalid JSON...) — typically a BookOrbit version without the
  // KOReader catalog API, or a proxy serving the web app at the API path. Lets the
  // UI distinguish "check your connection" from "check your server".
  static bool lastFetchBadResponse;

  /** Fetch the catalog root sections list. Returns false on any failure. */
  static bool fetchRootSections(std::vector<BookOrbitCatalogSection>& outSections);

  /**
   * Fetch the root sections' entry counts. The facet counts come from the server
   * dashboard, whose response also carries book lists the caller never reads; the
   * streaming filter drops them, so only the counts ever reach memory. The
   * dashboard's book counts include formats the catalog hides, so the two book
   * counts are instead the totals of their own EPUB listings: two more requests,
   * each for a single-item page. Returns false when the dashboard fails; a failed
   * book total only leaves that count unknown. Counts are decorative, so callers
   * treat any failure as "show no counts".
   */
  static bool fetchCatalogCounts(BookOrbitCatalogCounts& outCounts);

  /**
   * The listing query behind a root book section ("recent", "continue-reading",
   * "all-books"), mirroring the booksHref the server gives each of them, or the
   * want-to-read shelf.
   */
  static BookOrbitBookQuery sectionBookQuery(const std::string& sectionId);

  /**
   * Fetch a page of books matching the given filters.
   * @param query Sort and filter fields; empty fields are omitted
   * @param page 1-based page number
   */
  static bool fetchBooks(const BookOrbitBookQuery& query, int page, BookOrbitBookPage& outPage);

  /**
   * Fetch a page of a drill-down facet listing ("authors" or "series").
   * @param sectionId The facet section id from the catalog root
   * @param page 1-based page number
   */
  static bool fetchSectionEntries(const std::string& sectionId, int page, BookOrbitFacetPage& outPage);

  /** Fetch full detail (including downloadable files) for one book. */
  static bool fetchBookDetail(int64_t bookId, BookOrbitBookDetail& outDetail);

  /** Download a catalog file (by file id, not book id) to the SD card. */
  static HttpDownloader::DownloadError downloadFile(
      int64_t fileId, const std::string& destPath, HttpDownloader::ProgressCallback progress = nullptr,
      bool* cancelFlag = nullptr, HttpDownloader::DownloadOptions options = HttpDownloader::DownloadOptions());
};
