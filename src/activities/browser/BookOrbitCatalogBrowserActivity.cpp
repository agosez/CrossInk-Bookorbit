#include "BookOrbitCatalogBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "./BookOrbitCatalogListCache.h"
#include "BookOrbitCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace {
constexpr size_t BOOKORBIT_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr char READ_FOLDER_PREFIX[] = "/Read";
constexpr size_t MAX_LOCAL_ENTRIES = 200;
// Marker appended (right-aligned) to catalog rows whose book already exists on the
// device. U+2022 bullet: guaranteed by the built-in fonts' default glyph intervals.
constexpr char ON_DEVICE_MARKER[] = "\xE2\x80\xA2";

// The SD filename a catalog book downloads to; must stay in sync with downloadBook().
std::string catalogBookFilename(const std::string& title, const std::string& author) {
  const std::string suffix = author.empty() ? "" : (" - " + author);
  return "/" + StringUtils::sanitizeFilename(title + suffix) + ".epub";
}

// True when the catalog book already exists locally (download location or the
// /Read folder the finished-book move feature uses). Books manually moved into
// other folders are not detected — this is a best-effort convenience marker.
bool bookOnDevice(const std::string& title, const std::string& author) {
  const std::string filename = catalogBookFilename(title, author);
  if (Storage.exists(filename.c_str())) return true;
  const std::string readPath = std::string(READ_FOLDER_PREFIX) + filename;
  return Storage.exists(readPath.c_str());
}

bool hasEpubFile(const BookOrbitBookDetail& detail, BookOrbitCatalogFile& outFile) {
  for (const auto& file : detail.files) {
    std::string format = file.format;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return std::tolower(c); });
    if (format == "epub") {
      outFile = file;
      return true;
    }
  }
  return false;
}
}  // namespace

void BookOrbitCatalogBrowserActivity::onEnter() {
  Activity::onEnter();

  sdFontSystem.releaseLoadedFont(renderer);

  entries.clear();
  selectorIndex = 0;
  navLevel = NavLevel::Root;
  consumeConfirm = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  BookOrbitCatalogListCache::clear();

  if (!BOOKORBIT_STORE.hasCredentials()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_BOOKORBIT_SETUP_HINT);
    requestUpdate();
    return;
  }

  state = BrowserState::CHECK_WIFI;
  requestUpdate();
  checkAndConnectWifi();
}

void BookOrbitCatalogBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void BookOrbitCatalogBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    onWifiSelectionComplete(true);
    return;
  }
  launchWifiSelection();
}

void BookOrbitCatalogBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitCatalogBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    onGoHome();
    return;
  }
  sdFontSystem.releaseForNetwork(renderer);
  if (!loadRoot(/*allowNetwork=*/false)) {
    showLoadingBeforeFetch();
    loadRoot();
  }
}

void BookOrbitCatalogBrowserActivity::showLoadingBeforeFetch() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Loading screen could not be rendered before catalog fetch");
    requestUpdate(true);
  }
}

bool BookOrbitCatalogBrowserActivity::loadRoot(const bool allowNetwork) {
  navLevel = NavLevel::Root;
  std::vector<BookOrbitCatalogSection> sections;
  const bool cacheHit = BookOrbitCatalogListCache::loadRootSections(sections);
  if (!cacheHit && !allowNetwork) {
    return false;
  }
  if (!cacheHit && !BookOrbitCatalogClient::fetchRootSections(sections)) {
    state = BrowserState::ERROR;
    errorMessage = BookOrbitCatalogClient::lastFetchBadResponse ? tr(STR_BOOKORBIT_CATALOG_BAD_RESPONSE)
                                                                : tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return false;
  }
  if (!cacheHit) {
    BookOrbitCatalogListCache::saveRootSections(sections);
  }

  entries.clear();
  for (auto& section : sections) {
    Entry entry;
    const bool isFacet = section.id == "authors" || section.id == "series";
    entry.type = isFacet ? EntryType::FACET_SECTION : EntryType::SECTION;
    entry.title = section.title;
    entry.sectionId = section.id;
    entries.push_back(std::move(entry));
  }
  // Local, offline categories: what's already on the SD card.
  Entry onDevice;
  onDevice.type = EntryType::LOCAL_SECTION;
  onDevice.title = tr(STR_BOOKORBIT_ON_DEVICE);
  onDevice.sectionId = "on-device";
  entries.push_back(std::move(onDevice));
  Entry inProgress;
  inProgress.type = EntryType::LOCAL_SECTION;
  inProgress.title = tr(STR_BOOKORBIT_IN_PROGRESS);
  inProgress.sectionId = "in-progress";
  entries.push_back(std::move(inProgress));
  Entry search;
  search.type = EntryType::SEARCH;
  search.title = tr(STR_SEARCH);
  entries.push_back(std::move(search));

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
  return true;
}

void BookOrbitCatalogBrowserActivity::loadLocalBooks(const std::string& kind) {
  entries.clear();
  listTitle = (kind == "in-progress") ? tr(STR_BOOKORBIT_IN_PROGRESS) : tr(STR_BOOKORBIT_ON_DEVICE);

  if (kind == "in-progress") {
    // Books with local reading progress: the recent-books list minus finished ones.
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (!FsHelpers::hasEpubExtension(book.path) || !Storage.exists(book.path.c_str())) continue;
      const BookReadingStats stats = BookReadingStats::load(Epub::cachePathForFilePath(book.path, "/.crosspoint"));
      if (stats.isCompleted) continue;
      Entry entry;
      entry.type = EntryType::LOCAL_BOOK;
      entry.title = book.title.empty() ? book.path : book.title;
      entry.subtitle = book.author;
      entry.path = book.path;
      entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      if (FsHelpers::naturalLess(a.title, b.title)) return true;
      if (FsHelpers::naturalLess(b.title, a.title)) return false;
      return FsHelpers::naturalLess(a.subtitle, b.subtitle);
    });
  } else {
    // Every EPUB in the download location and the /Read folder, offline.
    const auto scanDir = [this](const char* dirPath) {
      FsFile dir = Storage.open(dirPath);
      if (!dir || !dir.isDirectory()) return;
      char name[128];
      FsFile file;
      while (entries.size() < MAX_LOCAL_ENTRIES && (file = dir.openNextFile())) {
        const size_t nameLen = file.isDirectory() ? 0 : file.getName(name, sizeof(name));
        file.close();
        if (nameLen > 5 && FsHelpers::hasEpubExtension(std::string_view(name, nameLen))) {
          Entry entry;
          entry.type = EntryType::LOCAL_BOOK;
          entry.title = std::string(name, nameLen - 5);  // strip ".epub"
          entry.path = (std::strcmp(dirPath, "/") == 0 ? std::string("/") : std::string(dirPath) + "/") + name;
          entries.push_back(std::move(entry));
        }
      }
      dir.close();
    };
    scanDir("/");
    scanDir(READ_FOLDER_PREFIX);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      if (FsHelpers::naturalLess(a.title, b.title)) return true;
      if (FsHelpers::naturalLess(b.title, a.title)) return false;
      return FsHelpers::naturalLess(a.subtitle, b.subtitle);
    });
  }

  // Local listings behave like a book list one level below the root. Neutralise
  // the paging context left by a previous server listing: without this, reaching
  // the bottom of a local list could append SERVER results into it, and the
  // scroll indicator would size itself on the stale server total.
  navLevel = NavLevel::Books;
  booksFromFacet = false;
  listPage = 1;
  listTotal = static_cast<int>(entries.size());
  listPageSize = std::max<int>(1, static_cast<int>(entries.size()));
  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

bool BookOrbitCatalogBrowserActivity::loadFacetEntries(const std::string& sectionId, const std::string& title,
                                                       const int page, const bool append, const bool allowNetwork) {
  BookOrbitFacetPage result;
  const bool cacheHit = BookOrbitCatalogListCache::loadFacetPage(sectionId, page, result);
  if (!cacheHit && !allowNetwork) {
    return false;
  }
  if (!cacheHit && !BookOrbitCatalogClient::fetchSectionEntries(sectionId, page, result)) {
    state = BrowserState::ERROR;
    errorMessage = BookOrbitCatalogClient::lastFetchBadResponse ? tr(STR_BOOKORBIT_CATALOG_BAD_RESPONSE)
                                                                : tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return false;
  }
  if (!cacheHit) {
    BookOrbitCatalogListCache::saveFacetPage(sectionId, page, result);
  }

  // Commit the navigation context only on success, so a failed page fetch leaves
  // the currently displayed list and its paging state consistent.
  navLevel = NavLevel::FacetList;
  facetSectionId = sectionId;
  facetTitle = title;
  facetPage = page;
  facetHasNext = result.hasNext;

  if (!append) {
    entries.clear();
  }
  // Server order is kept as-is: it is already alphabetical for facets, and a
  // per-page sort would break the overall order once pages are appended.
  for (auto& facet : result.entries) {
    Entry entry;
    entry.type = EntryType::FACET;
    entry.title = facet.title;
    if (facet.count > 0) {
      entry.title += " (" + std::to_string(facet.count) + ")";
    }
    entry.sectionId = facet.id;
    entry.seriesId = facet.seriesId;
    entries.push_back(std::move(entry));
  }
  if (!append) {
    selectorIndex = 0;
  }
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
  return true;
}

bool BookOrbitCatalogBrowserActivity::loadBooks(const BookOrbitBookQuery& query, const std::string& title,
                                                const int page, const bool fromFacet, const bool append,
                                                const bool allowNetwork) {
  BookOrbitBookPage result;
  const bool cacheHit = BookOrbitCatalogListCache::loadBooksPage(query, page, result);
  if (!cacheHit && !allowNetwork) {
    return false;
  }
  if (!cacheHit && !BookOrbitCatalogClient::fetchBooks(query, page, result)) {
    state = BrowserState::ERROR;
    errorMessage = BookOrbitCatalogClient::lastFetchBadResponse ? tr(STR_BOOKORBIT_CATALOG_BAD_RESPONSE)
                                                                : tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return false;
  }
  if (!cacheHit) {
    BookOrbitCatalogListCache::saveBooksPage(query, page, result);
  }

  // Commit the navigation context only on success (see loadFacetEntries).
  navLevel = NavLevel::Books;
  listQuery = query;
  listTitle = title;
  listPage = page;
  booksFromFacet = fromFacet;

  listTotal = result.total;
  listPageSize = result.pageSize > 0 ? result.pageSize : BookOrbitCatalogClient::PAGE_SIZE;

  if (!append) {
    entries.clear();
  }
  // Server order carries the listing's meaning -- recency for "Continue reading"
  // and "Recently added", series order for series -- and appending pages keeps
  // it consistent; a client-side re-sort would destroy both.
  for (auto& book : result.books) {
    Entry entry;
    entry.type = EntryType::BOOK;
    entry.title = book.title;
    entry.subtitle = book.author;
    entry.bookId = book.id;
    entry.onDevice = bookOnDevice(book.title, book.author);
    entries.push_back(std::move(entry));
  }
  if (!append) {
    selectorIndex = 0;
  }
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
  return true;
}

bool BookOrbitCatalogBrowserActivity::appendNextPageForCurrentList(const bool allowNetwork) {
  const size_t previousCount = entries.size();
  if (navLevel == NavLevel::FacetList && facetHasNext) {
    if (!loadFacetEntries(facetSectionId, facetTitle, facetPage + 1, true, /*allowNetwork=*/false)) {
      if (!allowNetwork) return false;
      showLoadingBeforeFetch();
      loadFacetEntries(facetSectionId, facetTitle, facetPage + 1, true);
    }
    return state == BrowserState::BROWSING && entries.size() > previousCount;
  }

  const bool booksHasNext = static_cast<long>(listPage) * listPageSize < listTotal;
  if (navLevel == NavLevel::Books && booksHasNext) {
    if (!loadBooks(listQuery, listTitle, listPage + 1, booksFromFacet, true, /*allowNetwork=*/false)) {
      if (!allowNetwork) return false;
      showLoadingBeforeFetch();
      loadBooks(listQuery, listTitle, listPage + 1, booksFromFacet, true);
    }
    return state == BrowserState::BROWSING && entries.size() > previousCount;
  }

  return false;
}

void BookOrbitCatalogBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void BookOrbitCatalogBrowserActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }
  BookOrbitBookQuery bookQuery;
  bookQuery.sort = "title";
  bookQuery.query = query;
  if (!loadBooks(bookQuery, query, 1, /*fromFacet=*/false, /*append=*/false, /*allowNetwork=*/false)) {
    showLoadingBeforeFetch();
    loadBooks(bookQuery, query, 1, /*fromFacet=*/false);
  }
}

void BookOrbitCatalogBrowserActivity::downloadBook(const int64_t bookId, const std::string& title) {
  state = BrowserState::DOWNLOADING;
  statusMessage = title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  BookOrbitBookDetail detail;
  if (!BookOrbitCatalogClient::fetchBookDetail(bookId, detail)) {
    state = BrowserState::ERROR;
    errorMessage = BookOrbitCatalogClient::lastFetchBadResponse ? tr(STR_BOOKORBIT_CATALOG_BAD_RESPONSE)
                                                                : tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return;
  }

  BookOrbitCatalogFile epubFile;
  if (!hasEpubFile(detail, epubFile)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_EPUB_FORMAT);
    requestUpdate();
    return;
  }

  const std::string filename = catalogBookFilename(detail.title, detail.author);
  LOG_DBG("BookOrbit", "Downloading file %lld -> %s", static_cast<long long>(epubFile.id), filename.c_str());

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested) return true;
    mappedInput.update();
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested = true;
    }
    return cancelRequested;
  };

  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = pollCancel;
  downloadOptions.bufferSize = BOOKORBIT_DOWNLOAD_BUFFER_SIZE;
  // TLS runs with a few KB of headroom on the C3 and reads can drop mid-transfer;
  // keep the partial file and resume instead of restarting a multi-MB download.
  downloadOptions.preservePartial = true;
  // Small client RX buffer (see BookOrbitCatalogClient::fetchJson) so the
  // headers-time body cache can't demand a large realloc next to the TLS buffers.
  downloadOptions.clientRxBufferSize = 2048;

  // Free the current listing while the download runs: every KB of contiguous heap
  // matters next to the TLS session, and the list is rebuilt from listQuery after.
  std::vector<Entry>().swap(entries);
  selectorIndex = 0;

  constexpr int MAX_DOWNLOAD_ATTEMPTS = 3;
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < MAX_DOWNLOAD_ATTEMPTS; attempt++) {
    // Honor a Back press between attempts too: a failing download otherwise runs
    // all its retries (TLS handshake included) with the cancel request ignored.
    if (pollCancel()) {
      result = HttpDownloader::ABORTED;
      break;
    }
    downloadOptions.resumePartial = attempt > 0;
    result = BookOrbitCatalogClient::downloadFile(
        epubFile.id, filename,
        [this](const size_t downloaded, const size_t total) {
          downloadProgress = downloaded;
          downloadTotal = total;
          requestUpdate(true);
        },
        &cancelRequested, downloadOptions);
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
    LOG_ERR("BookOrbit", "Download attempt %d/%d failed (err=%d), retrying", attempt + 1, MAX_DOWNLOAD_ATTEMPTS,
            static_cast<int>(result));
  }
  if (result != HttpDownloader::OK && result != HttpDownloader::ABORTED) {
    // preservePartial kept the partial file for resuming between attempts; don't
    // leave a truncated EPUB behind once we give up.
    Storage.remove(filename.c_str());
  }

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    // The listing was freed for download headroom; rebuild it from the stored
    // context so the user returns to the same page.
    if (!loadBooks(listQuery, listTitle, listPage, booksFromFacet, /*append=*/false, /*allowNetwork=*/false)) {
      showLoadingBeforeFetch();
      loadBooks(listQuery, listTitle, listPage, booksFromFacet);
    }
    return;
  } else if (result == HttpDownloader::ABORTED) {
    LOG_DBG("BookOrbit", "Download cancelled");
    mappedInput.suppressNextBackRelease();
    if (!loadBooks(listQuery, listTitle, listPage, booksFromFacet, /*append=*/false, /*allowNetwork=*/false)) {
      showLoadingBeforeFetch();
      loadBooks(listQuery, listTitle, listPage, booksFromFacet);
    }
    return;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

bool BookOrbitCatalogBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void BookOrbitCatalogBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    // Catalog browsing is a secondary feature: errors here just return you to the
    // previous list (or home from the root) rather than offering a retry, matching
    // the "no code beyond what's needed" scope decision for this feature.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (!BOOKORBIT_STORE.hasCredentials() || navLevel == NavLevel::Root) {
        onGoHome();
      } else if (entries.empty()) {
        // The listing was freed for a download that then failed; rebuild it.
        if (navLevel == NavLevel::FacetList) {
          if (!loadFacetEntries(facetSectionId, facetTitle, facetPage, /*append=*/false, /*allowNetwork=*/false)) {
            showLoadingBeforeFetch();
            loadFacetEntries(facetSectionId, facetTitle, facetPage);
          }
        } else {
          if (!loadBooks(listQuery, listTitle, listPage, booksFromFacet, /*append=*/false,
                         /*allowNetwork=*/false)) {
            showLoadingBeforeFetch();
            loadBooks(listQuery, listTitle, listPage, booksFromFacet);
          }
        }
      } else {
        state = BrowserState::BROWSING;
        requestUpdate();
      }
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        switch (entry.type) {
          case EntryType::SECTION: {
            BookOrbitBookQuery query;
            query.sort = entry.sectionId == "continue-reading" ? "recently_read"
                         : entry.sectionId == "all-books"      ? "title"
                                                               : "recently_added";
            if (!loadBooks(query, entry.title, 1, /*fromFacet=*/false, /*append=*/false, /*allowNetwork=*/false)) {
              showLoadingBeforeFetch();
              loadBooks(query, entry.title, 1, /*fromFacet=*/false);
            }
            break;
          }
          case EntryType::FACET_SECTION:
            if (!loadFacetEntries(entry.sectionId, entry.title, 1, /*append=*/false, /*allowNetwork=*/false)) {
              showLoadingBeforeFetch();
              loadFacetEntries(entry.sectionId, entry.title, 1);
            }
            break;
          case EntryType::LOCAL_SECTION:
            loadLocalBooks(entry.sectionId);
            break;
          case EntryType::LOCAL_BOOK:
            activityManager.goToReader(entry.path);
            break;
          case EntryType::FACET: {
            // Mirror BookOrbit's own plugin: author filters by the entry id; series
            // prefers the numeric seriesId and sorts by series order.
            BookOrbitBookQuery query;
            if (facetSectionId == "series") {
              query.sort = "series";
              if (!entry.seriesId.empty()) {
                query.seriesId = entry.seriesId;
              } else {
                query.series = entry.sectionId;
              }
            } else {
              query.author = entry.sectionId;
            }
            if (!loadBooks(query, entry.title, 1, /*fromFacet=*/true, /*append=*/false, /*allowNetwork=*/false)) {
              showLoadingBeforeFetch();
              loadBooks(query, entry.title, 1, /*fromFacet=*/true);
            }
            break;
          }
          case EntryType::SEARCH:
            launchSearch();
            break;
          case EntryType::BOOK:
            downloadBook(entry.bookId, entry.title);
            break;
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (navLevel == NavLevel::Root) {
        onGoHome();
      } else if (navLevel == NavLevel::Books && booksFromFacet) {
        if (!loadFacetEntries(facetSectionId, facetTitle, facetPage, /*append=*/false, /*allowNetwork=*/false)) {
          showLoadingBeforeFetch();
          loadFacetEntries(facetSectionId, facetTitle, facetPage);
        }
      } else {
        if (!loadRoot(/*allowNetwork=*/false)) {
          showLoadingBeforeFetch();
          loadRoot();
        }
      }
    }

    if (!entries.empty()) {
      // Same rows-per-page the themed list draws, so page jumps land where the
      // display pages; a fixed constant would drift from the theme's row height.
      const int pageItems = listPageItems();
      // More content on the server than is loaded? Then the loaded end is a
      // phantom boundary mid-listing: never wrap onto or past it.
      const auto hasMorePages = [this] {
        if (navLevel == NavLevel::FacetList) return facetHasNext;
        if (navLevel == NavLevel::Books) return static_cast<long>(listPage) * listPageSize < listTotal;
        return false;
      };
      // Prefetch at the page turn: after a forward move onto the last loaded
      // screen-page, append until that page is fully backed by loaded entries
      // (server pages are not screen-page multiples, and can even be smaller
      // than one screen). The load pause lands on a transition the e-ink
      // refreshes anyway instead of mid-page, and the page comes up full. The
      // lambdas read entries.size() live because appends grow the list.
      const auto extendIfOnLastPage = [this, pageItems, hasMorePages] {
        const int lastPageStart = (static_cast<int>(entries.size()) - 1) / pageItems * pageItems;
        if (selectorIndex < lastPageStart) return;
        const int wantedCount = (selectorIndex / pageItems + 1) * pageItems;
        while (static_cast<int>(entries.size()) < wantedCount && hasMorePages()) {
          if (!appendNextPageForCurrentList()) break;
        }
      };
      buttonNavigator.onNextRelease([this, extendIfOnLastPage, hasMorePages] {
        if (selectorIndex + 1 >= static_cast<int>(entries.size()) && hasMorePages() &&
            !appendNextPageForCurrentList()) {
          requestUpdate();  // could not load past the end (e.g. network); hold position, no wrap
          return;
        }
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        extendIfOnLastPage();
        requestUpdate();
      });
      // Backward from the very top: when the session cache holds the rest of the
      // listing, materialise it (no requests) and wrap to the real end. A cache
      // miss keeps the clamp -- a Previous press should not start a network crawl.
      const auto materialiseFromCache = [this, hasMorePages] {
        while (hasMorePages() && appendNextPageForCurrentList(/*allowNetwork=*/false)) {
        }
        return !hasMorePages();
      };
      buttonNavigator.onPreviousRelease([this, hasMorePages, materialiseFromCache] {
        if (selectorIndex == 0 && hasMorePages() && !materialiseFromCache()) return;
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this, pageItems, extendIfOnLastPage, hasMorePages] {
        const int count = static_cast<int>(entries.size());
        if (selectorIndex / pageItems == (count - 1) / pageItems && hasMorePages() && !appendNextPageForCurrentList()) {
          requestUpdate();
          return;
        }
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), pageItems);
        extendIfOnLastPage();
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this, pageItems, hasMorePages, materialiseFromCache] {
        if (selectorIndex / pageItems == 0 && hasMorePages()) {
          if (selectorIndex > 0) {
            selectorIndex = 0;  // finish the backward run at the top first
            requestUpdate();
            return;
          }
          if (!materialiseFromCache()) return;
        }
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
    }
  }
}

void BookOrbitCatalogBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string headerTitle = tr(STR_BOOKORBIT_CATALOG);
  if (navLevel == NavLevel::FacetList && !facetTitle.empty()) {
    headerTitle = facetTitle;
  } else if (navLevel == NavLevel::Books && !listTitle.empty()) {
    headerTitle = listTitle;
  }
  CompactHeader::drawTitle(renderer, headerTitle.c_str());

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 50, tr(STR_ERROR_MSG));
    // Error messages can be several lines long; drawCenteredText with an over-wide
    // string starts at a negative x and clips, so wrap it explicitly.
    const int messageWidth = pageWidth - 80;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), messageWidth, 4);
    int messageY = pageHeight / 2 - 20;
    for (const auto& line : messageLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineHeight + 4;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const bool onBook = !entries.empty() && entries[selectorIndex].type == EntryType::BOOK;
  const char* confirmLabel = onBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto entryCount = static_cast<int>(entries.size());

    const int contentTop = CompactHeader::contentTop(metrics);
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight;
    // The subtitle carries the author; without it two same-title search results
    // are indistinguishable. Passing the lambda also selects the taller row
    // height every themed list with subtitles uses.
    // Book listings tell the scroll indicator the server's total, so its size
    // and position are right from the first draw of a partially loaded list.
    const int scrollTotal = navLevel == NavLevel::Books ? listTotal : -1;
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, entryCount, selectorIndex,
        [this](int i) { return entries[i].title; }, [this](int i) { return entries[i].subtitle; },
        nullptr,  // rowIcon
        [this](int i) { return entries[i].onDevice ? ON_DEVICE_MARKER : ""; },
        /*highlightValue=*/false, /*rowDimmed=*/nullptr, /*isHeader=*/nullptr, /*rowHeightScale=*/1,
        /*showSelection=*/true, scrollTotal);
  }
  renderer.displayBuffer();
}

int BookOrbitCatalogBrowserActivity::listPageItems() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentHeight = renderer.getScreenHeight() - CompactHeader::contentTop(metrics) - metrics.buttonHintsHeight;
  return std::max(1, GUI.getListPageItems(contentHeight, /*hasSubtitle=*/true));
}
