#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <memory>
#include <string>

#include "BookOrbitCatalogDownload.h"
#include "BookOrbitCatalogLink.h"
#include "BookOrbitCatalogListOptions.h"
#include "BookOrbitCatalogViewport.h"
#include "BookOrbitCatalogWindow.h"
#include "activities/Activity.h"
#include "network/BookOrbitCatalogClient.h"
#include "util/ButtonNavigator.h"

class OptionPopup;
struct BookOrbitDownloadResume;

/**
 * Activity for browsing and downloading books from BookOrbit's catalog.
 *
 * Simplified compared to BookOrbit's own KOReader plugin: three navigation levels
 * (root sections -> optional authors/series/collections/libraries/smart-scopes
 * facet list -> a paged book list or a search), with each root section badged
 * with its entry count, the way the plugin badges its Browse tiles. A server book
 * list can be sorted and filtered by read status; the server does both, so paging
 * stays exact. Covers, ratings and read-status editing remain out of scope (see
 * SCOPE.md discussion for BookOrbit sync).
 *
 * The activity owns the session: Wi-Fi, which listing shows and how Back leaves it, the
 * input, and where a download ends. The rows and how they are kept in memory
 * (BookOrbitCatalogRows, BookOrbitCatalogWindow), what the list screen drew
 * (BookOrbitCatalogViewport), the sort and filter pickers (BookOrbitCatalogListOptions),
 * the transfer (BookOrbitCatalogDownload) and the screens around the listing
 * (BookOrbitCatalogScreens) each have a unit of their own.
 */
class BookOrbitCatalogBrowserActivity final : public Activity, private BookOrbitCatalogDownload::Reporter {
 public:
  // networkBoot: launched by a minimal network boot (BOOKORBIT_CATALOG or
  // BOOKORBIT_DOWNLOAD), which leaving the catalog must restart out of.
  // resumeDownload: the BOOKORBIT_DOWNLOAD boot, to run the download saved before the
  // restart (see downloadBook()).
  explicit BookOrbitCatalogBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           bool networkBoot = false, bool resumeDownload = false,
                                           BookOrbitCatalogLink link = {});
  ~BookOrbitCatalogBrowserActivity() override;  // defined where OptionPopup is complete

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  using Entry = BookOrbitCatalogEntry;
  using EntryType = BookOrbitCatalogEntry::Type;
  using PageLoad = BookOrbitCatalogWindow::PageLoad;
  using ListOption = BookOrbitCatalogListOptions::Picker;

  // FreeInkApp hosts the entry list (themed rows, touch routing); the compact
  // header keeps its own painter so the catalog's chrome stays as it was.
  // 24 interaction slots cover the densest page at the smallest UI scale, plus the
  // header's sort and filter controls on touch builds. Button-only builds keep 24: the
  // FreeInkApp<24> code is shared with other screens, a second size would cost ~7 KB.
  using UiApp = freeink::ui::FreeInkApp<CROSSINK_APP_CAP_TOUCH ? 26 : 24, 4>;

  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };
  // Which listing the BROWSING state currently shows; drives Back navigation.
  enum class NavLevel { Root, FacetList, Books };

  // The book list the Books level shows, used to page and to return to the same list
  // after a download.
  struct BookList {
    BookOrbitBookQuery query;
    // The read-status filter the listing opened with ("reading" for Continue reading): the
    // header names the filter only once the user picked another.
    std::string defaultReadStatus;
    std::string title;  // without a count: the header adds `total`
    int page = 1;       // the last page in memory
    int total = 0;
    int pageSize = BookOrbitCatalogClient::PAGE_SIZE;
    // Opened from a facet entry, so Back returns to the facet listing instead of the root.
    bool fromFacet = false;
    // An on-device list (On device, In progress): no server query behind it, so nothing
    // to sort or filter.
    bool local = false;
    // On device, paged from the files BookOrbitLocalBooks keeps on the card for it, which
    // go when the listing does.
    bool onDeviceFiles = false;
  };

  // The facet listing (authors, series...) the FacetList level shows.
  struct FacetList {
    std::string sectionId;
    std::string title;
    int page = 1;  // the last page in memory
    bool hasNext = false;
    // The row a book list was opened from, which Back returns to.
    int selectorIndex = 0;
    int topIndex = 0;
  };

  // The transfer a BOOKORBIT_DOWNLOAD boot runs once Wi-Fi is up; fileId 0 when none.
  struct PendingDownload {
    int64_t bookId = 0;
    int64_t fileId = 0;
    std::string title;
    std::string path;
    std::string variant;  // the file's downloadVariant
  };

  // --- Session --------------------------------------------------------------------
  // A minimal network boot never brought up the rest of the app, so leaving the
  // catalog from one restarts even when Wi-Fi never came up.
  const bool networkBoot;
  const bool resumeDownload;
  // Where the BookOrbit Home sent this session; consumed once Wi-Fi is up.
  BookOrbitCatalogLink entryLink;
  PendingDownload pendingDownload;
  BrowserState state = BrowserState::LOADING;
  std::string errorMessage;
  std::string statusMessage;  // the status line, or the title of the book downloading
  // The download was started from a BookOrbit Home shelf: whatever its outcome, it ends
  // on Home rather than on the listing it was fetched through.
  bool downloadFromHome = false;
  // The book a sync sent this session to update (BookOrbitCatalogLink::replacePath): the
  // session ends by reopening it rather than on Home, the download done or not.
  std::string reopenPath;
  bool goHomeAfterCancel = false;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  // --- Listing --------------------------------------------------------------------
  BookOrbitCatalogWindow window;
  NavLevel navLevel = NavLevel::Root;
  BookList bookList;
  FacetList facetList;
  // Every scroll position counts rows from the start of the listing (see
  // BookOrbitCatalogWindow), so a page dropped from the window changes neither.
  int selectorIndex = 0;
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  // Set while the listing is freed for a download's heap headroom, cleared once a
  // listing loads again. Distinguishes "rebuild the list Back should return to"
  // from a listing that genuinely loaded empty, where Back must navigate up
  // instead of reloading the same empty listing forever.
  bool listFreedForDownload = false;
  // Book counts of the on-device root entries, for their badges. On device is counted once
  // a session (-1 until then, and after a download or deletion changed it); In progress at
  // each root, and opening it reserves that many rows.
  int localOnDeviceCount = -1;
  size_t localInProgressCount = 0;

  // --- Input ----------------------------------------------------------------------
  ButtonNavigator buttonNavigator;
  bool consumeConfirm = false;
  // Set once a Confirm hold has opened the book action menu; input is swallowed
  // until Confirm is up again so its release does not also activate the row.
  bool longPressFired = false;
  // The sort or filter picker while it is open. Allocated for the pick and freed
  // right after, before the listing it changes is fetched: its strings and layout
  // must not sit in the heap the TLS session is about to need.
  std::unique_ptr<OptionPopup> listOptions;
  ListOption openListOption = ListOption::Sort;
  int pendingListChoice = -1;  // the row picked, applied once the picker has closed

  // --- Screen ---------------------------------------------------------------------
  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  BookOrbitCatalogViewport viewport;

  // Session: Wi-Fi, how it starts and how it ends.
  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void restoreFromResume(BookOrbitDownloadResume& resume);
  void leaveCatalog();
  void openLocalBook(const std::string& path);
  void showLoadingBeforeFetch();
  // The fetch failed: the error screen, telling a bad reply from no reply.
  void showFetchError();

  // Listings. The load* functions take one page from the list cache, or from the server
  // when allowNetwork; the show* ones try the cache first, then the server behind the
  // loading screen. All of them commit the navigation context only once the page loaded.
  bool loadRoot(bool allowNetwork);
  void showRoot();
  // By value: callers pass a row's own section id, and the listing it opens replaces that row.
  void loadLocalBooks(std::string kind);
  void loadOnDeviceBooks();
  // A page of the On device list, joining the window as a server page would (see PageLoad).
  bool loadOnDevicePage(int page, PageLoad load);
  bool loadFacetEntries(const std::string& sectionId, const std::string& title, int page, PageLoad load,
                        bool allowNetwork);
  bool showFacetPage(const std::string& sectionId, const std::string& title, int page, PageLoad load,
                     bool allowNetwork = true);
  bool loadBooks(const BookOrbitBookQuery& query, const std::string& title, int page, bool fromFacet, PageLoad load,
                 bool allowNetwork);
  bool showBookPage(const BookOrbitBookQuery& query, const std::string& title, int page, bool fromFacet, PageLoad load,
                    bool allowNetwork = true);
  // Opens a server book listing from a root section, a facet entry or a search.
  void openBookListing(const BookOrbitBookQuery& query, const std::string& title, bool fromFacet);
  void launchSearch();
  void performSearch(const std::string& query);
  // Opens the current book or facet listing again down to `lastPage`, walking its
  // pages in order so row numbers stay exact, then restores the scroll position.
  bool reopenListing(NavLevel level, int lastPage, int wantedSelector, int wantedTop);
  void restoreBookListAfterDownload();
  void navigateBack();
  void activateSelected();

  // The row window (see BookOrbitCatalogWindow).
  // Empties the rows for a new listing that will hold about `rows`, its scroll position
  // at row `firstRow`.
  void startListing(size_t rows, int firstRow = 0);
  // Room for `count` blank rows of page `page`, making room first so the window stays
  // within its reserve; returns the index of the first. Open starts the listing with its
  // first row in memory numbered firstRow.
  size_t joinPage(PageLoad load, size_t count, int page, int firstRow = 0);
  // BROWSING, or the error screen when the listing loaded empty.
  void settleListingState();
  bool hasMorePages() const;
  void clampSelectionToWindow();
  // One line per page load: what tells a field log how much heap browsing left.
  void logPageLoad(int page, bool cacheHit) const;
  bool appendNextPageForCurrentList(bool allowNetwork = true);
  bool prependPreviousPage();
  // Loads the pages in front of the window until `row` is in it.
  bool revealRow(int row);
  bool rewindToStart();
  // From the list cache, without requests: walks the rest of the listing and wraps
  // the selection to its end. A cache miss puts the listing back at its start.
  bool wrapToEndFromCache();

  // Input.
  void handleErrorBack();
  void handleBrowsingInput(bool backRequested);
  // Buttons move the selection and the viewport follows; swipes move the viewport.
  void moveThroughRows(bool listButtons);
  void selectNext(int pageItems);
  void selectPrevious();
  void pageForward(int pageItems);
  void pageBackward(int pageItems);
  void swipeRows(bool forward);
  // A move that could not reach its row (no network for a lost page) stays on the rows
  // in memory.
  void holdPosition();
  // A move past the rows in memory: earlier pages come back from the list cache, and a
  // wrap to the top reopens the listing at its first page.
  bool reachSelection();
  void followSelection();
  // Prefetch at the page turn: see the definition.
  void extendIfOnLastPage(int pageItems);
  // Rows a screen holds from topIndex: what the last frame drew, when sections made that
  // fewer than visibleRows.
  int screenRows() const;

  // Sort and filter (see BookOrbitCatalogListOptions).
  bool listOptionsAvailable() const;
  // The Library's list view setting: Expanded heads the list's sections, as it does there.
  bool sectionsShown() const;
  void showListOptions(ListOption option);
  // Runs the picker; true while it holds the input.
  bool handleListOptions();
  void applyListChoice(ListOption option, int choice);
  void reloadListing(const BookOrbitBookQuery& query);

  // Book actions.
  void showBookActionMenu(bool ignoreInitialConfirmRelease);
  void promptDeleteBook(int row);

  // Download.
  // replacePath: re-download over that existing on-device file instead of the
  // server's path; the old copy is only swapped out once the new one is complete.
  // openExisting: a file already at the server's path with the size of the library's file
  // is this book, put on the card by other means; it is opened instead (selecting a book).
  void downloadBook(int64_t bookId, const std::string& title, const std::string& replacePath = "",
                    bool openExisting = false);
  void showDownloadScreen(const std::string& title);
  // Saves the download and the listing, then restarts into the BOOKORBIT_DOWNLOAD
  // boot. False when the download runs in place instead: on S3 boards, and for a
  // BookOrbit Home shelf book in the catalog's network boot.
  bool restartForDownload(int64_t bookId, const BookOrbitCatalogFile& file, const std::string& title,
                          const std::string& path);
  void runPendingDownload(bool wifiConnected);
  // The transfer itself, then the listing it freed is rebuilt. variant: the file's downloadVariant.
  void transferBook(int64_t bookId, int64_t fileId, const std::string& variant, const std::string& finalPath);
  // BookOrbitCatalogDownload::Reporter
  void releaseForTransfer() override;
  void reportProgress(size_t downloaded, size_t total, bool redraw) override;
  bool cancelRequested() override;

  // Rendering.
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onControlEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  std::string headerTitle() const;
};
