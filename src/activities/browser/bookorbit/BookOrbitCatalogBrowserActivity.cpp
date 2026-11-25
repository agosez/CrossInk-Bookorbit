#include "BookOrbitCatalogBrowserActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "BookOrbitCatalogListCache.h"
#include "BookOrbitCatalogRows.h"
#include "BookOrbitCatalogScreens.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"
#include "BookOrbitLocalBooks.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CompactHeader.h"
#include "components/OptionPopup.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookOrbitIcons.h"
#include "components/icons/libraryIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
using Entry = BookOrbitCatalogEntry;
using EntryType = BookOrbitCatalogEntry::Type;

constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_CONTROL = 2;
constexpr int16_t HEADER_CONTROL_SORT = 0;
constexpr int16_t HEADER_CONTROL_FILTER = 1;
// Hold threshold for the book action menu (firmware convention, as in Recent Books).
constexpr unsigned long LONG_PRESS_MS = 1000;
// Marker appended (right-aligned) to catalog rows whose book already exists on the
// device. U+2022 bullet: guaranteed by the built-in fonts' default glyph intervals.
constexpr char ON_DEVICE_MARKER[] = "\xE2\x80\xA2";

// A server book list's header controls on touch devices: the Library's, minus its refresh
// and search. Sort, whose arrow shows the direction, then filter, right-aligned clear of
// the battery. A template, so a button-only build, whose call site is dead, never emits it.
template <typename Screen>
void addListHeaderControls(Screen& screen, const GfxRenderer& renderer, const bool descending) {
  using BookOrbitCatalogScreens::HEADER_CONTROL_GAP;
  using BookOrbitCatalogScreens::HEADER_CONTROL_SIZE;
  int bounds[4]{};
  renderer.getOrientedViewableTRBL(&bounds[0], &bounds[1], &bounds[2], &bounds[3]);
  const Rect header = TouchHeaderBackButton::compactHeaderRect(renderer);
  const int right = renderer.getScreenWidth() - bounds[1] - TouchHeaderBackButton::controlRightInset();
  const auto y = static_cast<int16_t>(header.y + header.height - HEADER_CONTROL_SIZE);
  fui::ButtonProps control;
  control.action = ACTION_CONTROL;
  control.inputMask = fui::InputTouch;
  control.styles = fui::plainStyles();
  control.icon = fui::bitmapFromIcon(descending ? icon_arrow_down_wide_narrow_32 : icon_arrow_up_narrow_wide_32);
  control.value = HEADER_CONTROL_SORT;
  screen.button(control, fui::Rect{static_cast<int16_t>(right - 2 * HEADER_CONTROL_SIZE - HEADER_CONTROL_GAP), y,
                                   HEADER_CONTROL_SIZE, HEADER_CONTROL_SIZE});
  control.icon = fui::bitmapFromIcon(icon_list_filter_32);
  control.value = HEADER_CONTROL_FILTER;
  screen.button(control, fui::Rect{static_cast<int16_t>(right - HEADER_CONTROL_SIZE), y, HEADER_CONTROL_SIZE,
                                   HEADER_CONTROL_SIZE});
}
}  // namespace

BookOrbitCatalogBrowserActivity::BookOrbitCatalogBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                                 const bool networkBoot, const bool resumeDownload,
                                                                 BookOrbitCatalogLink link)
    : Activity("BookOrbitCatalogBrowser", renderer, mappedInput),
      networkBoot(networkBoot || resumeDownload),
      resumeDownload(resumeDownload),
      entryLink(std::move(link)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

BookOrbitCatalogBrowserActivity::~BookOrbitCatalogBrowserActivity() = default;

// --- Session ----------------------------------------------------------------------

void BookOrbitCatalogBrowserActivity::onEnter() {
  Activity::onEnter();

  sdFontSystem.releaseLoadedFont(renderer);

  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitCatalogBrowserActivity::onRowEvent, this);
  if (mappedInput.hasTouchHardware()) app.on(ACTION_CONTROL, &BookOrbitCatalogBrowserActivity::onControlEvent, this);
  app.setScreen(&BookOrbitCatalogBrowserActivity::listScreen, this);

  selectorIndex = 0;
  navLevel = NavLevel::Root;
  bookList.local = false;
  bookList.onDeviceFiles = false;
  localOnDeviceCount = -1;
  bookList.defaultReadStatus.clear();
  listOptions.reset();
  pendingListChoice = -1;
  consumeConfirm = false;
  longPressFired = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  pendingDownload = PendingDownload{};

  // A download boot comes back to the listing it left, whose pages are still in the
  // list cache; any other entry starts a fresh catalog session.
  BookOrbitDownloadResume resume;
  if (resumeDownload && BookOrbitCatalogListCache::takeDownloadResume(resume)) {
    restoreFromResume(resume);
  } else {
    BookOrbitCatalogListCache::clear();
    // A BOOKORBIT_CATALOG boot the BookOrbit Home asked for brings its link on the SD card.
    if (networkBoot && entryLink.bookId == 0 && entryLink.sectionId.empty()) {
      BookOrbitCatalogListCache::takeEntryLink(entryLink);
    }
  }

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
  listOptions.reset();
  window.rows().clear();
  if (bookList.onDeviceFiles) BookOrbitLocalBooks::clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  } else if (networkBoot) {
    silentRestart();
  }
}

// The listing a download boot comes back to, and the download it runs once Wi-Fi is up.
void BookOrbitCatalogBrowserActivity::restoreFromResume(BookOrbitDownloadResume& resume) {
  navLevel = NavLevel::Books;
  bookList.query = std::move(resume.listQuery);
  bookList.defaultReadStatus = std::move(resume.listDefaultReadStatus);
  bookList.title = std::move(resume.listTitle);
  bookList.page = resume.listPage;
  bookList.fromFacet = resume.booksFromFacet;
  selectorIndex = resume.selectorIndex;
  topIndex = resume.topIndex;
  facetList.sectionId = std::move(resume.facetSectionId);
  facetList.title = std::move(resume.facetTitle);
  facetList.page = resume.facetPage;
  facetList.selectorIndex = resume.facetSelectorIndex;
  facetList.topIndex = resume.facetTopIndex;
  downloadFromHome = resume.fromHome;
  pendingDownload = {resume.bookId, resume.fileId, std::move(resume.title), std::move(resume.path)};
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
  if (pendingDownload.fileId != 0) {
    runPendingDownload(connected);
    return;
  }
  if (!connected) {
    leaveCatalog();
    return;
  }
  sdFontSystem.releaseForNetwork(renderer);
  if (entryLink.bookId != 0) {
    // Back from the download lands on the listing the Home shelf drew from.
    const BookOrbitCatalogLink link = std::move(entryLink);
    entryLink = BookOrbitCatalogLink{};
    navLevel = NavLevel::Books;
    bookList.query = BookOrbitCatalogClient::sectionBookQuery("recent");
    bookList.title = link.title;
    bookList.page = 1;
    bookList.fromFacet = false;
    downloadFromHome = true;
    // A sync sent this one to replace the open book's outdated copy: back to that book after,
    // whatever the outcome.
    reopenPath = link.replacePath;
    // A shelf book is selected as a catalog row is: a copy already at its place opens.
    downloadBook(link.bookId, link.title, link.replacePath, /*openExisting=*/link.replacePath.empty());
    return;
  }
  showRoot();
  // A linked section opens over the root, which Back returns to.
  if (!entryLink.sectionId.empty()) {
    const BookOrbitCatalogLink link = std::move(entryLink);
    entryLink = BookOrbitCatalogLink{};
    if (BookOrbitCatalogRows::isFacetSection(link.sectionId)) {
      loadFacetEntries(link.sectionId, link.title, 1, PageLoad::Open, /*allowNetwork=*/true);
    } else {
      openBookListing(BookOrbitCatalogClient::sectionBookQuery(link.sectionId), link.title, /*fromFacet=*/false);
    }
  }
}

void BookOrbitCatalogBrowserActivity::leaveCatalog() {
  if (reopenPath.empty()) {
    onGoHome();
  } else {
    openLocalBook(reopenPath);
  }
}

void BookOrbitCatalogBrowserActivity::openLocalBook(const std::string& path) {
  // Pushing the reader here would never survive: onExit() reboots while the
  // radio is still up, so the reader is torn down before onEnter() records
  // the book. Persist the path and let the reboot land on it instead.
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  // The restart skips onExit(): the On device list's files go now.
  if (bookList.onDeviceFiles) BookOrbitLocalBooks::clear();
  silentRestartToReader();
}

void BookOrbitCatalogBrowserActivity::showLoadingBeforeFetch() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Loading screen could not be rendered before catalog fetch");
    requestUpdate(true);
  }
}

void BookOrbitCatalogBrowserActivity::showFetchError() {
  state = BrowserState::ERROR;
  errorMessage = BookOrbitCatalogClient::lastFetchBadResponse ? tr(STR_BOOKORBIT_CATALOG_BAD_RESPONSE)
                                                              : tr(STR_BOOKORBIT_CATALOG_ERROR);
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

// --- Listings ---------------------------------------------------------------------

bool BookOrbitCatalogBrowserActivity::loadRoot(const bool allowNetwork) {
  navLevel = NavLevel::Root;
  // The On device list's files on the card belong to the listing being left.
  if (bookList.onDeviceFiles) {
    BookOrbitLocalBooks::clear();
    bookList.onDeviceFiles = false;
  }
  std::vector<BookOrbitCatalogSection> sections;
  const bool cacheHit = BookOrbitCatalogListCache::loadRootSections(sections);
  if (!cacheHit && !allowNetwork) {
    return false;
  }
  if (!cacheHit && !BookOrbitCatalogClient::fetchRootSections(sections)) {
    showFetchError();
    return false;
  }
  if (!cacheHit) {
    BookOrbitCatalogListCache::saveRootSections(sections);
  }

  // Per-section entry counts, the way BookOrbit's own plugin badges its Browse
  // tiles: the dashboard plus the totals of the two EPUB book listings, cached
  // with the listings. Non-fatal — an older server without the dashboard just
  // shows the sections without counts.
  BookOrbitCatalogCounts counts;
  if (!BookOrbitCatalogListCache::loadCatalogCounts(counts) && allowNetwork &&
      BookOrbitCatalogClient::fetchCatalogCounts(counts)) {
    BookOrbitCatalogListCache::saveCatalogCounts(counts);
  }

  // Local, offline categories: what's already on the SD card. Counting On device walks the
  // index and three folders, opening every indexed book, so it is done once a session and
  // again by opening the list: only this catalog's own downloads and deletions change it.
  if (localOnDeviceCount < 0) localOnDeviceCount = static_cast<int>(BookOrbitLocalBooks::count());
  localInProgressCount = BookOrbitCatalogRows::collectInProgress(nullptr);

  RenderLock lock(*this);  // the render task draws the rows replaced here
  startListing(sections.size() + BookOrbitCatalogRows::ROOT_EXTRA_ROWS);
  listFreedForDownload = false;
  BookOrbitCatalogRows::appendRoot(window.rows(), sections, counts, localOnDeviceCount, localInProgressCount);
  settleListingState();
  lock.unlock();
  requestUpdate();
  return true;
}

void BookOrbitCatalogBrowserActivity::showRoot() {
  if (!loadRoot(/*allowNetwork=*/false)) {
    showLoadingBeforeFetch();
    loadRoot(/*allowNetwork=*/true);
  }
}

void BookOrbitCatalogBrowserActivity::loadLocalBooks(const std::string kind) {
  if (kind != "in-progress") {
    loadOnDeviceBooks();
    return;
  }
  RenderLock lock(*this);  // the render task draws the rows replaced here
  bookList.title = tr(STR_BOOKORBIT_IN_PROGRESS);
  // Sized by the root's badge count: the recent-books list bounds this one, small enough
  // to sort whole, and growing it a row at a time would double its block.
  startListing(localInProgressCount);

  BookOrbitCatalogRows::collectInProgress(&window.rows());
  BookOrbitCatalogRows::sortByTitle(window.rows());

  // Local listings behave like a book list one level below the root. Neutralise
  // the paging context left by a previous server listing: without this, reaching
  // the bottom of a local list could append SERVER results into it, and the
  // scroll indicator would size itself on the stale server total.
  const int rows = static_cast<int>(window.rows().size());
  navLevel = NavLevel::Books;
  bookList.fromFacet = false;
  bookList.local = true;
  listFreedForDownload = false;
  bookList.page = 1;
  bookList.total = rows;
  bookList.pageSize = std::max<int>(1, rows);
  settleListingState();
  lock.unlock();
  requestUpdate();
}

// On device can list every book on the card, so it is not held whole: BookOrbitLocalBooks
// sorts it on the card, and its pages come into the row window the way a server listing's
// do, a server-sized page at a time.
void BookOrbitCatalogBrowserActivity::loadOnDeviceBooks() {
  showLoadingBeforeFetch();
  int total = 0;
  const bool built = BookOrbitLocalBooks::build(total);
  {
    RenderLock lock(*this);
    bookList.title = tr(STR_BOOKORBIT_ON_DEVICE);
    // A book list one level below the root, with no server query behind it (see above).
    navLevel = NavLevel::Books;
    bookList.fromFacet = false;
    bookList.local = true;
    listFreedForDownload = false;
    bookList.onDeviceFiles = built;
    bookList.page = 1;
    bookList.total = total;
    bookList.pageSize = BookOrbitCatalogClient::PAGE_SIZE;
  }
  if (built) localOnDeviceCount = total;
  if (built && total > 0 && loadOnDevicePage(1, PageLoad::Open)) return;

  RenderLock lock(*this);
  startListing(0);
  state = BrowserState::ERROR;
  errorMessage = built && total == 0 ? tr(STR_NO_ENTRIES) : tr(STR_BOOKORBIT_ON_DEVICE_ERROR);
  lock.unlock();
  requestUpdate();
}

bool BookOrbitCatalogBrowserActivity::loadOnDevicePage(const int page, const PageLoad load) {
  const int firstRow = (page - 1) * bookList.pageSize;
  const int count = std::min(bookList.pageSize, bookList.total - firstRow);
  if (page < 1 || count <= 0) return false;
  std::vector<std::string> paths;
  if (!BookOrbitLocalBooks::readPage(firstRow, static_cast<size_t>(count), paths)) return false;

  // The window's inserts and erases move the rows the render task draws.
  RenderLock lock(*this);
  if (load != PageLoad::Prepend) bookList.page = page;
  // The wrap to the end opens the listing at its last page, rows numbered from there.
  const size_t first = joinPage(load, paths.size(), page, firstRow);
  for (size_t i = 0; i < paths.size(); i++) {
    BookOrbitCatalogRows::setLocalBook(window.rows()[first + i], std::move(paths[i]));
  }
  LOG_INF("BookOrbit", "On device page %d: %u rows in memory from row %d of %d (free=%u maxAlloc=%u)", page,
          static_cast<unsigned>(window.rows().size()), window.start(), bookList.total, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  state = BrowserState::BROWSING;
  lock.unlock();
  requestUpdate();
  return true;
}

bool BookOrbitCatalogBrowserActivity::loadFacetEntries(const std::string& sectionId, const std::string& title,
                                                       const int page, const PageLoad load, const bool allowNetwork) {
  BookOrbitFacetPage result;
  const bool cacheHit = BookOrbitCatalogListCache::loadFacetPage(sectionId, page, result);
  if (!cacheHit && !allowNetwork) {
    return false;
  }
  if (!cacheHit && !BookOrbitCatalogClient::fetchSectionEntries(sectionId, page, result)) {
    showFetchError();
    return false;
  }
  if (!cacheHit) {
    BookOrbitCatalogListCache::saveFacetPage(sectionId, page, result);
  }

  // Commit the navigation context only on success, so a failed page fetch leaves
  // the currently displayed list and its paging state consistent. The arguments can
  // be the rows' own strings, so they are copied before the rows change. The render
  // task draws these rows, and the window's inserts and erases move them: hold it off.
  RenderLock lock(*this);
  navLevel = NavLevel::FacetList;
  listFreedForDownload = false;
  facetList.sectionId = sectionId;
  facetList.title = title;
  // The window's last page says whether more follow; a page put in front changes neither.
  if (load != PageLoad::Prepend) {
    facetList.page = page;
    facetList.hasNext = result.hasNext;
  }

  // Server order is kept as-is: it is already alphabetical for facets, and a
  // per-page sort would break the overall order once pages are appended.
  const size_t first = joinPage(load, result.entries.size(), page);
  for (size_t i = 0; i < result.entries.size(); i++) {
    BookOrbitCatalogRows::setFacet(window.rows()[first + i], std::move(result.entries[i]));
  }
  logPageLoad(page, cacheHit);
  settleListingState();
  lock.unlock();
  requestUpdate();
  return true;
}

bool BookOrbitCatalogBrowserActivity::showFacetPage(const std::string& sectionId, const std::string& title,
                                                    const int page, const PageLoad load, const bool allowNetwork) {
  if (loadFacetEntries(sectionId, title, page, load, /*allowNetwork=*/false)) return true;
  if (!allowNetwork) return false;
  showLoadingBeforeFetch();
  return loadFacetEntries(sectionId, title, page, load, /*allowNetwork=*/true);
}

bool BookOrbitCatalogBrowserActivity::loadBooks(const BookOrbitBookQuery& query, const std::string& title,
                                                const int page, const bool fromFacet, const PageLoad load,
                                                const bool allowNetwork) {
  BookOrbitBookPage result;
  const bool cacheHit = BookOrbitCatalogListCache::loadBooksPage(query, page, result);
  if (!cacheHit && !allowNetwork) {
    return false;
  }
  if (!cacheHit && !BookOrbitCatalogClient::fetchBooks(query, page, result)) {
    showFetchError();
    return false;
  }
  if (!cacheHit) {
    BookOrbitCatalogListCache::saveBooksPage(query, page, result);
  }
  // Which of the page's books are on the card, before the render lock: one pass over the
  // download index for the whole page. A page of strings, runtime-sized: heap.
  auto devicePaths = makeUniqueNoThrow<std::string[]>(result.books.size());
  if (devicePaths) {
    BookOrbitCatalogDownload::findOnDevice(result.books, devicePaths.get());
  } else {
    LOG_ERR("BookOrbit", "Out of memory marking a page's books on the device");
  }

  // Commit the navigation context only on success (see loadFacetEntries).
  RenderLock lock(*this);
  navLevel = NavLevel::Books;
  listFreedForDownload = false;
  bookList.query = query;
  bookList.title = title;
  if (load != PageLoad::Prepend) bookList.page = page;
  bookList.fromFacet = fromFacet;
  bookList.local = false;

  bookList.total = result.total;
  bookList.pageSize = result.pageSize > 0 ? result.pageSize : BookOrbitCatalogClient::PAGE_SIZE;

  // Server order carries the listing's meaning -- recency for "Continue reading"
  // and "Recently added", series order for series -- and appending pages keeps
  // it consistent; a client-side re-sort would destroy both.
  const size_t first = joinPage(load, result.books.size(), page);
  for (size_t i = 0; i < result.books.size(); i++) {
    BookOrbitCatalogRows::setBook(window.rows()[first + i], std::move(result.books[i]),
                                  devicePaths ? std::move(devicePaths[i]) : std::string());
  }
  // The letters span the whole listing and only come with its first page.
  if (page == 1) window.setLetters(result.letters);
  logPageLoad(page, cacheHit);
  settleListingState();
  lock.unlock();
  requestUpdate();
  return true;
}

bool BookOrbitCatalogBrowserActivity::showBookPage(const BookOrbitBookQuery& query, const std::string& title,
                                                   const int page, const bool fromFacet, const PageLoad load,
                                                   const bool allowNetwork) {
  if (loadBooks(query, title, page, fromFacet, load, /*allowNetwork=*/false)) return true;
  if (!allowNetwork) return false;
  showLoadingBeforeFetch();
  return loadBooks(query, title, page, fromFacet, load, /*allowNetwork=*/true);
}

void BookOrbitCatalogBrowserActivity::openBookListing(const BookOrbitBookQuery& query, const std::string& title,
                                                      const bool fromFacet) {
  {
    // Only the root and facet listings open a book listing, and their header does not
    // read this; the lock covers a render that would.
    RenderLock lock(*this);
    bookList.defaultReadStatus = query.readStatus;
  }
  showBookPage(query, title, 1, fromFacet, PageLoad::Open);
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
  openBookListing(bookQuery, query, /*fromFacet=*/false);
}

bool BookOrbitCatalogBrowserActivity::reopenListing(const NavLevel level, const int lastPage, const int wantedSelector,
                                                    const int wantedTop) {
  for (int page = 1; page <= lastPage; page++) {
    const PageLoad load = page == 1 ? PageLoad::Open : PageLoad::Append;
    const bool loaded = level == NavLevel::FacetList
                            ? showFacetPage(facetList.sectionId, facetList.title, page, load)
                            : showBookPage(bookList.query, bookList.title, page, bookList.fromFacet, load);
    if (!loaded) {
      return false;  // the load set the error state; keep whatever pages did load
    }
    // Open put the listing at its top. The wanted viewport is what decides which
    // pages the rest of the walk drops.
    if (page == 1) topIndex = wantedTop;
  }
  if (window.empty()) return false;
  topIndex = std::clamp(wantedTop, window.start(), window.end() - 1);
  selectorIndex = std::clamp(wantedSelector, window.start(), window.end() - 1);
  return true;
}

// Rebuild the book listing that downloadBook() freed, and put the selector back on
// the entry it was on
void BookOrbitCatalogBrowserActivity::restoreBookListAfterDownload() {
  reopenListing(NavLevel::Books, bookList.page, selectorIndex, topIndex);
}

void BookOrbitCatalogBrowserActivity::navigateBack() {
  if (navLevel == NavLevel::Root) {
    leaveCatalog();
  } else if (navLevel == NavLevel::Books && bookList.fromFacet) {
    reopenListing(NavLevel::FacetList, facetList.page, facetList.selectorIndex, facetList.topIndex);
  } else {
    showRoot();
  }
}

void BookOrbitCatalogBrowserActivity::activateSelected() {
  const Entry* selected = window.at(selectorIndex);
  if (!selected) return;
  // A reference into the rows: the loads below copy what they need before replacing them.
  const auto& entry = *selected;
  switch (entry.type) {
    case EntryType::SECTION:
      openBookListing(BookOrbitCatalogClient::sectionBookQuery(entry.sectionId),
                      BookOrbitCatalogRows::titleWithoutBadge(entry), /*fromFacet=*/false);
      break;
    case EntryType::FACET_SECTION:
      showFacetPage(entry.sectionId, entry.title, 1, PageLoad::Open);
      break;
    case EntryType::LOCAL_SECTION:
      loadLocalBooks(entry.sectionId);
      break;
    case EntryType::LOCAL_BOOK:
      openLocalBook(entry.path);
      break;
    case EntryType::FACET:
      // Back reopens the facet listing on this row.
      facetList.selectorIndex = selectorIndex;
      facetList.topIndex = topIndex;
      openBookListing(BookOrbitCatalogRows::facetBookQuery(facetList.sectionId, entry),
                      BookOrbitCatalogRows::titleWithoutBadge(entry), /*fromFacet=*/true);
      break;
    case EntryType::SEARCH:
      launchSearch();
      break;
    case EntryType::BOOK:
      if (entry.onDevice) {
        openLocalBook(entry.path);
      } else {
        downloadBook(entry.bookId, entry.title, "", /*openExisting=*/true);
      }
      break;
  }
}

// --- Row window -------------------------------------------------------------------

void BookOrbitCatalogBrowserActivity::startListing(const size_t rows, const int firstRow) {
  window.reset(rows, firstRow);
  selectorIndex = firstRow;
  topIndex = firstRow;
  // What the render task measured belongs to the previous listing.
  viewport.newListing();
}

size_t BookOrbitCatalogBrowserActivity::joinPage(const PageLoad load, const size_t count, const int page,
                                                 const int firstRow) {
  if (load == PageLoad::Open) startListing(count, firstRow);
  // Make room before the page arrives, so the window stays within its reserve: trimmed
  // after, it briefly holds one page too many and the vector moves to a larger block.
  if (load == PageLoad::Append) {
    window.dropPagesBefore(topIndex, /*incoming=*/1);
    clampSelectionToWindow();
  }
  if (load == PageLoad::Prepend) {
    const int lastPage = window.dropPagesAfter(topIndex + visibleRows - 1, /*incoming=*/1);
    if (lastPage > 0) {
      // The window now ends a page earlier, and the server still has that page after it.
      if (navLevel == NavLevel::FacetList) {
        facetList.page = lastPage;
        facetList.hasNext = true;
      } else {
        bookList.page = lastPage;
      }
    }
    clampSelectionToWindow();
  }
  return window.placePage(load, count, static_cast<uint16_t>(page));
}

void BookOrbitCatalogBrowserActivity::settleListingState() {
  state = window.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (window.empty()) errorMessage = tr(STR_NO_ENTRIES);
}

bool BookOrbitCatalogBrowserActivity::hasMorePages() const {
  if (navLevel == NavLevel::FacetList) return facetList.hasNext;
  if (navLevel == NavLevel::Books) return static_cast<long>(bookList.page) * bookList.pageSize < bookList.total;
  return false;
}

// A swipe leaves the selection where it was while the viewport moves on, so a dropped
// page can take the selected row with it; the selection then holds the nearest row.
void BookOrbitCatalogBrowserActivity::clampSelectionToWindow() { selectorIndex = window.clamp(selectorIndex); }

void BookOrbitCatalogBrowserActivity::logPageLoad(const int page, const bool cacheHit) const {
  LOG_INF("BookOrbit", "Listing page %d from %s: %u rows in memory from row %d (free=%u maxAlloc=%u)", page,
          cacheHit ? "cache" : "server", static_cast<unsigned>(window.rows().size()), window.start(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
}

bool BookOrbitCatalogBrowserActivity::appendNextPageForCurrentList(const bool allowNetwork) {
  const int previousEnd = window.end();
  if (navLevel == NavLevel::FacetList && facetList.hasNext) {
    return showFacetPage(facetList.sectionId, facetList.title, facetList.page + 1, PageLoad::Append, allowNetwork) &&
           state == BrowserState::BROWSING && window.end() > previousEnd;
  }
  if (navLevel == NavLevel::Books && hasMorePages()) {
    if (bookList.onDeviceFiles)
      return loadOnDevicePage(bookList.page + 1, PageLoad::Append) && window.end() > previousEnd;
    return showBookPage(bookList.query, bookList.title, bookList.page + 1, bookList.fromFacet, PageLoad::Append,
                        allowNetwork) &&
           state == BrowserState::BROWSING && window.end() > previousEnd;
  }
  return false;
}

// The pages in front of the window were shown earlier in this session, so the list
// cache has them; the network only stands in for a page the cache failed to keep.
bool BookOrbitCatalogBrowserActivity::prependPreviousPage() {
  if (window.firstPage() <= 1) return false;  // also an empty window
  const int page = window.firstPage() - 1;
  const int previousStart = window.start();
  if (navLevel == NavLevel::FacetList) {
    showFacetPage(facetList.sectionId, facetList.title, page, PageLoad::Prepend);
  } else if (navLevel == NavLevel::Books && bookList.onDeviceFiles) {
    if (!loadOnDevicePage(page, PageLoad::Prepend)) return false;
  } else if (navLevel == NavLevel::Books) {
    showBookPage(bookList.query, bookList.title, page, bookList.fromFacet, PageLoad::Prepend);
  } else {
    return false;
  }
  return state == BrowserState::BROWSING && window.start() < previousStart;
}

bool BookOrbitCatalogBrowserActivity::revealRow(const int row) {
  while (row < window.start()) {
    if (!prependPreviousPage()) return false;
  }
  return true;
}

bool BookOrbitCatalogBrowserActivity::rewindToStart() {
  if (navLevel == NavLevel::FacetList) return showFacetPage(facetList.sectionId, facetList.title, 1, PageLoad::Open);
  if (navLevel == NavLevel::Books) {
    if (bookList.onDeviceFiles) return loadOnDevicePage(1, PageLoad::Open);
    return showBookPage(bookList.query, bookList.title, 1, bookList.fromFacet, PageLoad::Open);
  }
  return false;
}

bool BookOrbitCatalogBrowserActivity::wrapToEndFromCache() {
  if (bookList.onDeviceFiles) {
    // The card has every page: open the last one rather than walk there.
    if (!loadOnDevicePage((bookList.total - 1) / bookList.pageSize + 1, PageLoad::Open)) return false;
    topIndex = std::max(window.start(), window.end() - visibleRows);
    return true;
  }
  while (hasMorePages()) {
    // The walk keeps nothing behind it: the selection lands on the listing's end.
    topIndex = selectorIndex = window.end() - 1;
    if (!appendNextPageForCurrentList(/*allowNetwork=*/false)) break;
  }
  if (hasMorePages()) {
    if (window.start() > 0) {
      rewindToStart();
    } else {
      selectorIndex = topIndex = 0;
    }
    return false;
  }
  topIndex = std::max(window.start(), window.end() - visibleRows);
  return true;
}

// --- Input ------------------------------------------------------------------------

void BookOrbitCatalogBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (handleListOptions()) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }

  // The compact header carries a back button on touch devices. It sits above the
  // app's content margin, so no list row can claim the same tap.
  const bool backRequested =
      mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer));

  switch (state) {
    case BrowserState::ERROR:
      if (backRequested) handleErrorBack();
      return;
    case BrowserState::CHECK_WIFI:
    case BrowserState::LOADING:
      if (backRequested) leaveCatalog();
      return;
    case BrowserState::BROWSING:
      handleBrowsingInput(backRequested);
      return;
    case BrowserState::DOWNLOADING:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::SEARCH_INPUT:
      return;
  }
}

void BookOrbitCatalogBrowserActivity::handleErrorBack() {
  // Catalog browsing is a secondary feature: errors here just return you to the
  // previous list (or home from the root) rather than offering a retry, matching
  // the "no code beyond what's needed" scope decision for this feature.
  if (!BOOKORBIT_STORE.hasCredentials() || navLevel == NavLevel::Root || downloadFromHome) {
    leaveCatalog();
  } else if (window.empty() && listFreedForDownload) {
    // The listing was freed for a download that then failed; rebuild it.
    restoreBookListAfterDownload();
  } else if (window.empty()) {
    // The listing genuinely loaded empty (a section with no entries, a search
    // with no matches): reloading it would show the same error forever, so
    // Back navigates up, exactly as it does from a non-empty listing.
    navigateBack();
  } else {
    state = BrowserState::BROWSING;
    requestUpdate();
  }
}

void BookOrbitCatalogBrowserActivity::handleBrowsingInput(const bool backRequested) {
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
    return;
  }
  // Holding Confirm on a catalog book opens its action menu instead of acting.
  const Entry* selected = window.at(selectorIndex);
  if (selected && selected->type == EntryType::BOOK && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    showBookActionMenu(/*ignoreInitialConfirmRelease=*/true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  if (backRequested) {
    navigateBack();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent or onControlEvent
      if (state != BrowserState::BROWSING) return;
    }
  }

  // A server book list on a button-only device takes the Library's layout: Left sorts,
  // Right filters, and Up and Down alone move through the rows.
  const bool listButtons = listOptionsAvailable() && !mappedInput.hasTouchHardware();
  if (listButtons) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      showListOptions(ListOption::Sort);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      showListOptions(ListOption::Filter);
      return;
    }
  }
  if (!window.empty()) moveThroughRows(listButtons);
}

void BookOrbitCatalogBrowserActivity::moveThroughRows(const bool listButtons) {
  using NavButtons = std::array<MappedInputManager::Button, 2>;
  const NavButtons nextButtons = listButtons
                                     ? NavButtons{MappedInputManager::Button::Down, MappedInputManager::Button::Down}
                                     : ButtonNavigator::getNextButtons();
  const NavButtons previousButtons = listButtons
                                         ? NavButtons{MappedInputManager::Button::Up, MappedInputManager::Button::Up}
                                         : ButtonNavigator::getPreviousButtons();

  // Section headings can move the screen further down than this loop put it; start
  // from where the last frame really was.
  if (sectionsShown()) topIndex = viewport.adoptedTop(topIndex, selectorIndex, window.start(), window.end());
  // Rows the list actually drew, so page jumps land where the display pages;
  // a fixed constant would drift from the theme's row height. Every index below
  // counts rows from the start of the listing (see BookOrbitCatalogWindow), so the
  // pages a move loads or drops leave the screen-page boundaries where they were.
  const int pageItems = visibleRows;

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    swipeRows(/*forward=*/swipe == MappedInputManager::SwipeDir::Up);
    return;
  }
  buttonNavigator.onRelease(nextButtons, [this, pageItems] { selectNext(pageItems); });
  buttonNavigator.onRelease(previousButtons, [this] { selectPrevious(); });
  buttonNavigator.onContinuous(nextButtons, [this, pageItems] { pageForward(pageItems); });
  buttonNavigator.onContinuous(previousButtons, [this, pageItems] { pageBackward(pageItems); });
}

void BookOrbitCatalogBrowserActivity::selectNext(const int pageItems) {
  if (selectorIndex + 1 >= window.end() && hasMorePages() && !appendNextPageForCurrentList()) {
    requestUpdate();  // could not load past the end (e.g. network); hold position, no wrap
    return;
  }
  selectorIndex = ButtonNavigator::nextIndex(selectorIndex, window.end());
  if (!reachSelection()) return holdPosition();
  extendIfOnLastPage(pageItems);
  followSelection();
  requestUpdate();
}

// Backward from the very top: when the session cache holds the rest of the
// listing, walk it (no requests) and wrap to the real end. A cache miss keeps
// the clamp -- a Previous press should not start a network crawl.
void BookOrbitCatalogBrowserActivity::selectPrevious() {
  const int from = selectorIndex;
  if (from == 0 && hasMorePages() && !wrapToEndFromCache()) return holdPosition();
  selectorIndex = ButtonNavigator::previousIndex(from, window.end());
  if (!reachSelection()) return holdPosition();
  followSelection();
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::pageForward(const int pageItems) {
  if (selectorIndex / pageItems == (window.end() - 1) / pageItems && hasMorePages() &&
      !appendNextPageForCurrentList()) {
    requestUpdate();
    return;
  }
  selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, window.end(), pageItems);
  if (!reachSelection()) return holdPosition();
  extendIfOnLastPage(pageItems);
  followSelection();
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::pageBackward(const int pageItems) {
  const int from = selectorIndex;
  if (from / pageItems == 0 && hasMorePages()) {
    if (from > 0) {
      selectorIndex = 0;  // finish the backward run at the top first
      requestUpdate();
      return;
    }
    if (!wrapToEndFromCache()) return holdPosition();
  }
  selectorIndex = ButtonNavigator::previousPageIndex(from, window.end(), pageItems);
  if (!reachSelection()) return holdPosition();
  followSelection();
  requestUpdate();
}

// Swipes move the viewport and leave the selection where it is. Reaching the end of
// what is loaded pulls the next page in, so a finger can walk a long listing the way
// the buttons already do.
void BookOrbitCatalogBrowserActivity::swipeRows(const bool forward) {
  if (forward) {
    const int wantedCount = topIndex + 2 * visibleRows;
    while (window.end() < wantedCount && hasMorePages()) {
      if (!appendNextPageForCurrentList()) break;
    }
  }
  // A screen with section headings holds fewer rows: the next one starts where it ended.
  const int rowsOnScreen = screenRows();
  const int delta = forward ? rowsOnScreen : -rowsOnScreen;
  const int previousTop = topIndex;
  topIndex = scrollListBy(topIndex, delta, visibleRows, window.end());
  if (!revealRow(topIndex)) topIndex = window.start();
  if (topIndex != previousTop) requestUpdate();
}

void BookOrbitCatalogBrowserActivity::holdPosition() {
  clampSelectionToWindow();
  requestUpdate();
}

bool BookOrbitCatalogBrowserActivity::reachSelection() {
  if (selectorIndex >= window.start()) return true;
  return selectorIndex == 0 ? rewindToStart() : revealRow(selectorIndex);
}

void BookOrbitCatalogBrowserActivity::followSelection() {
  topIndex = followListSelection(selectorIndex, topIndex, visibleRows, window.end());
  if (!revealRow(topIndex)) topIndex = window.start();
}

// After a forward move onto the last loaded screen-page, append until that page is fully
// backed by loaded entries (server pages are not screen-page multiples, and can even be
// smaller than one screen). The load pause lands on a transition the e-ink refreshes
// anyway instead of mid-page, and the page comes up full.
void BookOrbitCatalogBrowserActivity::extendIfOnLastPage(const int pageItems) {
  const int lastPageStart = (window.end() - 1) / pageItems * pageItems;
  if (selectorIndex < lastPageStart) return;
  const int wantedCount = (selectorIndex / pageItems + 1) * pageItems;
  while (window.end() < wantedCount && hasMorePages()) {
    if (!appendNextPageForCurrentList()) break;
  }
}

int BookOrbitCatalogBrowserActivity::screenRows() const { return viewport.screenRows(topIndex, visibleRows); }

// --- Sort and filter --------------------------------------------------------------

bool BookOrbitCatalogBrowserActivity::listOptionsAvailable() const {
  // A shelf comes in the server's own order, which nothing on the request changes.
  return state == BrowserState::BROWSING && navLevel == NavLevel::Books && !bookList.local &&
         bookList.query.shelf.empty();
}

bool BookOrbitCatalogBrowserActivity::sectionsShown() const {
  return SETTINGS.libraryListExpanded != 0 && navLevel == NavLevel::Books && !bookList.local;
}

void BookOrbitCatalogBrowserActivity::showListOptions(const ListOption option) {
  auto popup = makeUniqueNoThrow<OptionPopup>();
  if (!popup) {
    LOG_ERR("BookOrbit", "Cannot allocate the listing's %s picker", option == ListOption::Sort ? "sort" : "filter");
    return;
  }
  // Only records the pick: applying it fetches a listing, which must not run inside
  // the picker's input handling (it holds the render lock).
  BookOrbitCatalogListOptions::show(*popup, option, bookList.query,
                                    [this](const int choice) { pendingListChoice = choice; });
  {
    RenderLock lock(*this);  // the render task draws the picker
    openListOption = option;
    pendingListChoice = -1;
    listOptions = std::move(popup);
  }
  requestUpdate();
}

bool BookOrbitCatalogBrowserActivity::handleListOptions() {
  if (!listOptions) return false;
  {
    RenderLock lock(*this);
    listOptions->handleInput(mappedInput, [this] { requestUpdate(); });
    if (listOptions->isActive()) return true;
    // Closed: picked or dismissed. Its strings go now, before a pick fetches the listing.
    listOptions.reset();
  }
  const int choice = std::exchange(pendingListChoice, -1);
  if (choice >= 0) applyListChoice(openListOption, choice);
  return true;
}

void BookOrbitCatalogBrowserActivity::applyListChoice(const ListOption option, const int choice) {
  BookOrbitBookQuery query;
  switch (BookOrbitCatalogListOptions::apply(option, choice, bookList.query, query)) {
    case BookOrbitCatalogListOptions::Change::None:
      return;
    case BookOrbitCatalogListOptions::Change::ListView:
      // The Library's own setting: its list shows the same sections, or not.
      SETTINGS.libraryListExpanded = SETTINGS.libraryListExpanded ? 0 : 1;
      if (!SETTINGS.saveToFile()) LOG_ERR("BookOrbit", "Cannot save the list view");
      requestUpdate();
      return;
    case BookOrbitCatalogListOptions::Change::Listing:
      reloadListing(query);
      return;
  }
}

void BookOrbitCatalogBrowserActivity::reloadListing(const BookOrbitBookQuery& query) {
  // The listing on screen stays in memory until the new one has loaded, so a failed
  // request ends on the error screen, whose Back returns to it unchanged.
  if (!showBookPage(query, bookList.title, 1, bookList.fromFacet, PageLoad::Open)) return;
  if (!window.empty()) return;
  // A filter can leave nothing to show. Stay on the listing, whose pickers can undo it,
  // rather than on the error screen, whose Back would leave it.
  {
    RenderLock lock(*this);
    state = BrowserState::BROWSING;
  }
  requestUpdate();
}

// --- Book actions -----------------------------------------------------------------

void BookOrbitCatalogBrowserActivity::showBookActionMenu(const bool ignoreInitialConfirmRelease) {
  const Entry* selected = window.at(selectorIndex);
  if (!selected || selected->type != EntryType::BOOK) return;
  const Entry& entry = *selected;

  std::vector<FileBrowserActionActivity::MenuItem> items;
  if (entry.onDevice) {
    items.push_back({FileBrowserAction::Open, StrId::STR_OPEN});
    items.push_back({FileBrowserAction::Redownload, StrId::STR_REDOWNLOAD});
    items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  } else {
    items.push_back({FileBrowserAction::Download, StrId::STR_DOWNLOAD});
  }

  // Captured by row: the menu is modal, so the listing cannot change under it.
  const int row = selectorIndex;
  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, entry.title,
                                                                     std::move(items), ignoreInitialConfirmRelease),
                         [this, row](const ActivityResult& result) {
                           longPressFired = false;
                           const Entry* current = window.at(row);
                           if (result.isCancelled || !current) return;
                           const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
                           if (!actionResult) {
                             LOG_ERR("BookOrbit", "Book action result missing");
                             return;
                           }
                           // Copies: downloadBook() frees the listing these would point into.
                           const Entry entry = *current;
                           LOG_DBG("BookOrbit", "Book action %d on book %lld", actionResult->action,
                                   static_cast<long long>(entry.bookId));
                           switch (static_cast<FileBrowserAction>(actionResult->action)) {
                             case FileBrowserAction::Open:
                               openLocalBook(entry.path);
                               return;
                             case FileBrowserAction::Download:
                               downloadBook(entry.bookId, entry.title);
                               return;
                             case FileBrowserAction::Redownload:
                               downloadBook(entry.bookId, entry.title, entry.path);
                               return;
                             case FileBrowserAction::Delete:
                               promptDeleteBook(row);
                               return;
                             default:
                               return;
                           }
                         });
}

void BookOrbitCatalogBrowserActivity::promptDeleteBook(const int row) {
  const Entry* book = window.at(row);
  if (!book) return;
  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book->title),
                         [this, row](const ActivityResult& result) {
                           Entry* current = window.at(row);
                           if (result.isCancelled || !current) return;
                           Entry& entry = *current;
                           BookActions::clearFileMetadata(entry.path);
                           if (!Storage.remove(entry.path.c_str())) {
                             LOG_ERR("BookOrbit", "Failed to delete book: %s", entry.path.c_str());
                             return;
                           }
                           RECENT_BOOKS.removeByPath(entry.path);
                           library::invalidateLibraryIndex();
                           localOnDeviceCount = -1;  // recounted by the next root
                           // Re-detect rather than just clearing the flag: another copy (the SD root,
                           // /Read) may still match. The index drops its now-stale entry on lookup.
                           entry.onDevice = BookOrbitCatalogDownload::findOnDevice(entry.bookId, entry.title,
                                                                                   entry.subtitle, entry.path);
                           requestUpdate();
                         });
}

// --- Download ---------------------------------------------------------------------

void BookOrbitCatalogBrowserActivity::showDownloadScreen(const std::string& title) {
  state = BrowserState::DOWNLOADING;
  // Truncate once, up front: render() used to re-truncate this unchanging title on
  // every progress repaint, an avoidable heap allocation racing the TLS session's
  // own tight budget mid-download (see HttpDownloader heap notes).
  statusMessage = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), renderer.getScreenWidth() - 40);
  downloadProgress = downloadTotal = 0;
  goHomeAfterCancel = false;
  requestUpdate(true);
}

void BookOrbitCatalogBrowserActivity::downloadBook(const int64_t bookId, const std::string& title,
                                                   const std::string& replacePath, const bool openExisting) {
  showDownloadScreen(title);

  BookOrbitBookDetail detail;
  if (!BookOrbitCatalogClient::fetchBookDetail(bookId, detail)) {
    showFetchError();
    return;
  }

  BookOrbitCatalogFile epubFile;
  if (!BookOrbitCatalogDownload::findEpubFile(detail, epubFile)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_EPUB_FORMAT);
    requestUpdate();
    return;
  }

  // A re-download keeps the book where it is: its reading progress and caches are
  // keyed by that path. Either way the transfer is staged in a side file and only
  // swapped in once it is complete and opens as an EPUB (see
  // BookOrbitCatalogDownload::transfer), so a failure or a cancel never costs the user
  // the copy they had, and a new book never sits half-written under its own name.
  const std::string finalPath =
      replacePath.empty() ? BookOrbitCatalogDownload::destination(detail, epubFile) : replacePath;
  // A copy already at that path opens instead of downloading again (on the X3/X4, a
  // restart too), and the index records it for the next listings. A different size
  // downloads, and the update replaces it.
  if (openExisting && BookOrbitCatalogDownload::isAlreadyAt(finalPath, epubFile)) {
    LOG_INF("BookOrbit", "Book %lld is already at %s, opening it", static_cast<long long>(bookId), finalPath.c_str());
    BookOrbitDownloadIndex::record(bookId, finalPath);
    openLocalBook(finalPath);
    return;
  }
  if (!BookOrbitCatalogDownload::makeFolders(finalPath)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  if (restartForDownload(bookId, epubFile.id, title, finalPath)) return;
  transferBook(bookId, epubFile.id, finalPath);
}

bool BookOrbitCatalogBrowserActivity::restartForDownload(const int64_t bookId, const int64_t fileId,
                                                         const std::string& title, const std::string& path) {
#if FREEINK_MCU_C3
  // A BookOrbit Home shelf opens the catalog's network boot straight onto its book, and
  // that session ends on Home without browsing. Only the book's details were fetched, so
  // the heap is still the clean one a download boot gives: a second restart would only
  // add a second Wi-Fi connection.
  if (networkBoot && downloadFromHome) {
    LOG_INF("BookOrbit", "Downloading book %lld in the catalog's network boot, without restarting",
            static_cast<long long>(bookId));
    return false;
  }
  BookOrbitDownloadResume resume;
  resume.bookId = bookId;
  resume.fileId = fileId;
  resume.title = title;
  resume.path = path;
  resume.listQuery = bookList.query;
  resume.listDefaultReadStatus = bookList.defaultReadStatus;
  resume.listTitle = bookList.title;
  resume.listPage = bookList.page;
  resume.booksFromFacet = bookList.fromFacet;
  resume.selectorIndex = selectorIndex;
  resume.topIndex = topIndex;
  resume.facetSectionId = facetList.sectionId;
  resume.facetTitle = facetList.title;
  resume.facetPage = facetList.page;
  resume.facetSelectorIndex = facetList.selectorIndex;
  resume.facetTopIndex = facetList.topIndex;
  resume.fromHome = downloadFromHome;
  if (!BookOrbitCatalogListCache::saveDownloadResume(resume)) {
    LOG_ERR("BookOrbit", "Cannot save the download for its restart; downloading in place");
    return false;
  }
  LOG_INF("BookOrbit", "Restarting into the download of book %lld (row %d, top %d, page %d)",
          static_cast<long long>(bookId), selectorIndex, topIndex, bookList.page);
  silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_DOWNLOAD);
  // Only returns when deep sleep is already under way, which ends this session anyway.
  BookOrbitCatalogListCache::discardDownloadResume();
  return true;
#else
  (void)bookId;
  (void)fileId;
  (void)title;
  (void)path;
  return false;
#endif
}

void BookOrbitCatalogBrowserActivity::runPendingDownload(const bool wifiConnected) {
  const PendingDownload job = std::move(pendingDownload);
  pendingDownload = PendingDownload{};
  if (!wifiConnected) {
    // Backing out of Wi-Fi cancels the download: back to where it was started.
    if (downloadFromHome) {
      leaveCatalog();
      return;
    }
    restoreBookListAfterDownload();
    requestUpdate();
    return;
  }
  showDownloadScreen(job.title);
  transferBook(job.bookId, job.fileId, job.path);
}

void BookOrbitCatalogBrowserActivity::transferBook(const int64_t bookId, const int64_t fileId,
                                                   const std::string& finalPath) {
  const HttpDownloader::DownloadError result = BookOrbitCatalogDownload::transfer(bookId, fileId, finalPath, *this);
  const bool downloadFailed = result != HttpDownloader::OK && result != HttpDownloader::ABORTED;

  if (result == HttpDownloader::OK) {
    localOnDeviceCount = -1;  // recounted by the next root
    if (!reopenPath.empty()) {
      openLocalBook(finalPath);
      return;
    }
  } else if (result == HttpDownloader::ABORTED) {
    LOG_DBG("BookOrbit", "Download cancelled");
    if (goHomeAfterCancel) {
      leaveCatalog();
      return;
    }
    mappedInput.suppressNextBackRelease();
  } else {
    LOG_ERR("BookOrbit", "Download failed (err=%d)", static_cast<int>(result));
    errorMessage = result == HttpDownloader::INSUFFICIENT_SPACE ? tr(STR_SD_CARD_FULL) : tr(STR_DOWNLOAD_FAILED);
  }

  // A download from Home goes back there once done or cancelled; a failure first shows
  // its message, which Back dismisses to Home as well.
  if (downloadFromHome && !downloadFailed) {
    leaveCatalog();
    return;
  }

  // The listing was freed for download headroom; rebuild it from the stored
  // context so the user returns to the same page, on the same book.
  restoreBookListAfterDownload();

  // The rebuild leaves BROWSING state behind, which would swallow the failure
  if (downloadFailed) {
    state = BrowserState::ERROR;
  }

  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::releaseForTransfer() {
  // Free the current listing while the download runs: every KB of contiguous heap
  // matters next to the TLS session, and the list is rebuilt from bookList after.
  RenderLock lock(*this);
  window.release();
  listFreedForDownload = true;
}

void BookOrbitCatalogBrowserActivity::reportProgress(const size_t downloaded, const size_t total, const bool redraw) {
  downloadProgress = downloaded;
  downloadTotal = total;
  if (redraw) requestUpdate(true);
}

bool BookOrbitCatalogBrowserActivity::cancelRequested() {
  mappedInput.update();
  bool cancel = false;
  if (mappedInput.wasHomeGesture()) {
    goHomeAfterCancel = true;
    cancel = true;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel = true;
  }
  return cancel;
}

// --- Rendering --------------------------------------------------------------------

void BookOrbitCatalogBrowserActivity::onControlEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitCatalogBrowserActivity*>(user);
  self->app.clearTapFlash();
  if (!self->listOptionsAvailable()) return;
  self->showListOptions(event.value == HEADER_CONTROL_FILTER ? ListOption::Filter : ListOption::Sort);
}

void BookOrbitCatalogBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitCatalogBrowserActivity*>(user);
  // The list was given the rows in memory, so its action values index them.
  const int row = self->window.start() + event.value;
  const Entry* entry = self->window.at(row);
  if (!entry) return;
  self->selectorIndex = row;
  // Activation loads a new listing or starts a download; a lingering flash
  // would gray an unrelated row of whatever comes next.
  self->app.clearTapFlash();
  if (event.longPress && entry->type == EntryType::BOOK) {
    self->showBookActionMenu(/*ignoreInitialConfirmRelease=*/false);
    return;
  }
  self->activateSelected();
}

void BookOrbitCatalogBrowserActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookOrbitCatalogBrowserActivity*>(user)->buildListScreen(screen);
}

void BookOrbitCatalogBrowserActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the compact header band, above the button hints. The header
  // is painted by render(), so the app must not claim its rows.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(CompactHeader::contentTop(metrics)), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  if (mappedInput.hasTouchHardware() && listOptionsAvailable()) {
    addListHeaderControls(screen, renderer, BookOrbitCatalogListOptions::sortsDescending(bookList.query));
  }
  // A filter that matches nothing leaves the header controls to undo it; render() says
  // the listing is empty.
  if (window.empty()) return;

  const auto& rows = window.rows();
  const bool sections = sectionsShown();
  // Transient per-render: points into the rows, freed on scope exit. The list gets the
  // rows in memory, so its indices are those rows' own (row - window.start()).
  std::vector<fui::ListItem> items;
  items.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    const auto& entry = rows[i];
    fui::ListItem item;
    item.label = entry.title.c_str();
    // The subtitle carries the author; without it two same-title search results
    // are indistinguishable.
    if (!entry.subtitle.empty()) item.subtitle = entry.subtitle.c_str();
    if (entry.onDevice) item.value = ON_DEVICE_MARKER;
    if (sections) item.sectionHeading = window.heading(i);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  const int selected = window.at(selectorIndex) ? selectorIndex - window.start() : -1;
  props.selectedIndex = static_cast<int16_t>(selected);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);  // physical buttons stay in loop()
  props.valueInset = 8;  // air between the on-device dot and the row edge
  if (sections) configureUiListSectionHeaders(props, screen.theme());
  const auto rowsPerScreen = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rowsPerScreen > 0 ? rowsPerScreen : 1;
  // Clamped for this frame only: the loop owns topIndex, and a listing being rebuilt
  // around a wanted position (reopenListing) must not see it pulled into a partial window.
  // With sections the fixed-height clamp would pull back a top the last frame moved down
  // to fit the listing's end: only the window bounds it.
  const int top =
      viewport.passTop(sections ? std::clamp(topIndex, window.start(), window.end() - 1)
                                : std::max(window.start(), scrollListBy(topIndex, 0, visibleRows, window.end())));
  props.topIndex = static_cast<uint16_t>(top - window.start());
  // A nav only to measure: it makes the list skip its fixed-height clamp too.
  if (sections) props.nav = viewport.measureNav();

  const fui::Rect listRect = screen.body();
  screen.list(props);

  const BookOrbitCatalogViewport::Pass drawn{top,         selected,       window.start(), static_cast<int>(rows.size()),
                                             visibleRows, !hasMorePages()};
  if (viewport.endPass(drawn, sections)) return;  // render() draws the frame again further down

  // A long listing keeps only a few pages in memory, so the list sized its
  // indicator on that window: a 500-book listing would look like it ends one swipe
  // away, and a window deep in it like its top. Repaint the same track against the
  // whole listing instead -- the server's total for books, what has been reached
  // for facets, which give no total. Only when the list already drew one: that is
  // what shrank the rows to clear the track.
  const int scrollTotal = navLevel == NavLevel::Books ? std::max(bookList.total, window.end()) : window.end();
  if ((window.start() > 0 || scrollTotal > window.end()) && static_cast<int>(rows.size()) > visibleRows) {
    const auto& theme = screen.theme();
    fui::drawListScrollIndicator(screen.target(), listRect, static_cast<uint32_t>(scrollTotal),
                                 static_cast<uint32_t>(visibleRows), static_cast<uint32_t>(top), theme.listScrollWidth,
                                 theme.listScrollSide, theme.listScrollInset);
  }
}

std::string BookOrbitCatalogBrowserActivity::headerTitle() const {
  if (navLevel == NavLevel::FacetList && !facetList.title.empty()) return facetList.title;
  if (navLevel != NavLevel::Books || bookList.title.empty()) return tr(STR_BOOKORBIT_CATALOG);
  std::string title = bookList.title;
  if (!bookList.local && !bookList.query.readStatus.empty() &&
      bookList.query.readStatus != bookList.defaultReadStatus) {
    title += " \xC2\xB7 ";  // U+00B7 middle dot, in the built-in fonts' Latin-1 range
    title += BookOrbitCatalogListOptions::readStatusLabel(bookList.query.readStatus);
  }
  // The listing's own total: a facet badge counts every format, and no badge knows the
  // filter. Only once it is on screen; a download boot draws this header before it loads.
  if (state == BrowserState::BROWSING) title += " (" + std::to_string(bookList.total) + ")";
  return title;
}

void BookOrbitCatalogBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const bool listOptionsShown = listOptionsAvailable();
  const std::string title = headerTitle();
  const auto drawHeader = [this, &title, listOptionsShown] {
    BookOrbitCatalogScreens::drawHeader(renderer, mappedInput, title.c_str(), listOptionsShown);
  };
  drawHeader();

  // WIFI_SELECTION: the Wi-Fi screen is only pushed on the next loop, and a network
  // boot paints the catalog before that, with no listing loaded yet.
  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING || state == BrowserState::WIFI_SELECTION) {
    BookOrbitCatalogScreens::drawStatus(renderer, mappedInput, statusMessage.c_str());
    renderer.displayBuffer();
    return;
  }
  if (state == BrowserState::ERROR) {
    BookOrbitCatalogScreens::drawError(renderer, mappedInput, errorMessage.c_str());
    renderer.displayBuffer();
    return;
  }
  if (state == BrowserState::DOWNLOADING) {
    BookOrbitCatalogScreens::drawDownload(renderer, mappedInput, statusMessage.c_str(), downloadProgress,
                                          downloadTotal);
    renderer.displayBuffer();
    return;
  }

  if (!window.empty() || listOptionsShown) {
    uiReady = false;
    viewport.beginFrame(topIndex, selectorIndex);
    // A pass that found the selection, or the listing's end, below the screen asks for
    // another one further down (see buildListScreen); each starts from a blank frame.
    for (;;) {
      app.render();
      if (!viewport.takeAnotherPass()) break;
      renderer.clearScreen();
      drawHeader();
    }
    uiReady = true;
  }
  if (window.empty()) renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_NO_ENTRIES));
  // The picker draws its own button hints over the listing, then pushes the frame.
  if (listOptions && listOptions->processRender(renderer, mappedInput)) return;

  const Entry* selected = window.at(selectorIndex);
  const bool onRemoteBook = selected && selected->type == EntryType::BOOK && !selected->onDevice;
  const char* confirmLabel = !selected ? "" : onRemoteBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  BookOrbitCatalogScreens::drawListHints(renderer, mappedInput, confirmLabel,
                                         listOptionsShown && !mappedInput.hasTouchHardware());
  renderer.displayBuffer();
}
