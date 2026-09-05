#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "network/HttpDownloader.h"

/**
 * A top-level BookOrbit catalog section (e.g. "Recently added", "Continue reading",
 * "All books", or the authors/series/collections drill-down facets). BookOrbit's
 * library/smart-scope browsing facets are not supported, matching CrossInk's
 * simple browse experience for OPDS.
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
};

/** One entry in a BookOrbit book listing. */
struct BookOrbitCatalogBook {
  int64_t id = 0;
  std::string title;
  std::string author;  // first author only, for compact list display
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
  std::string sort;          // BookOrbit sort id (e.g. "recently_added", "title", "series")
  std::string query;         // free-text search
  std::string author;        // author filter (facet entry id)
  std::string seriesId;      // numeric series filter (preferred when present)
  std::string series;        // series-name filter (fallback when no seriesId)
  std::string collectionId;  // numeric collection filter (facet entry id)
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
 * authors/series/collections drill-down facets, and downloading an EPUB file from
 * a book's detail. Library/smart-scope drill-down, covers, ratings and
 * read-status editing are out of scope.
 */
class BookOrbitCatalogClient {
 public:
  static constexpr int PAGE_SIZE = 20;

  // True when the last failed fetch reached a server but got a non-catalog reply
  // (HTML page, invalid JSON...) — typically a BookOrbit version without the
  // KOReader catalog API, or a proxy serving the web app at the API path. Lets the
  // UI distinguish "check your connection" from "check your server".
  static bool lastFetchBadResponse;

  /** Fetch the catalog root sections list. Returns false on any failure. */
  static bool fetchRootSections(std::vector<BookOrbitCatalogSection>& outSections);

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
