#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitCatalogLink.h"
#include "activities/Activity.h"
#include "network/BookOrbitCatalogClient.h"
#include "util/ButtonNavigator.h"

class OptionPopup;

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
 */
class BookOrbitCatalogBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };
  enum class EntryType { SECTION, FACET_SECTION, FACET, LOCAL_SECTION, LOCAL_BOOK, SEARCH, BOOK };
  // Which listing the BROWSING state currently shows; drives Back navigation.
  enum class NavLevel { Root, FacetList, Books };

  struct Entry {
    EntryType type;
    std::string title;
    std::string subtitle;  // author, for BOOK/LOCAL_BOOK entries
    // Section id (SECTION/FACET_SECTION/LOCAL_SECTION), facet entry id (FACET), or for a
    // BOOK the list section its listing's sort files it under (its series, the day it was
    // added), empty when that sort has none.
    std::string sectionId;
    std::string seriesId;  // numeric series id, for FACET entries of the series facet
    std::string path;      // SD path: LOCAL_BOOK entries, and BOOK entries already on the device
    int64_t bookId = 0;
    bool onDevice = false;  // BOOK entries: a matching file already exists on the device (at `path`)
    uint16_t page = 0;      // server page the row came from; 0 for the root and the on-device lists
    // Length of `title` before its " (count)" badge, 0 when it has none. A listing opened
    // from the row is titled without it: its header counts what the listing holds.
    uint16_t badgeAt = 0;
  };

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

 private:
  // FreeInkApp hosts the entry list (themed rows, touch routing); the compact
  // header keeps its own painter so the catalog's chrome stays as it was.
  // 24 interaction slots cover the densest page at the smallest UI scale, plus the
  // header's sort and filter controls on touch builds. Button-only builds keep 24: the
  // FreeInkApp<24> code is shared with other screens, a second size would cost ~7 KB.
  using UiApp = freeink::ui::FreeInkApp<CROSSINK_APP_CAP_TOUCH ? 26 : 24, 4>;

  // The two pickers of a server book list. Sort and Left on button-only devices, Filter
  // and Right; a header control each on touch devices.
  enum class ListOption : uint8_t { Sort, Filter };

  // The transfer a BOOKORBIT_DOWNLOAD boot runs once Wi-Fi is up; fileId 0 when none.
  struct PendingDownload {
    int64_t bookId = 0;
    int64_t fileId = 0;
    std::string title;
    std::string path;
  };

  // How a server page joins the rows in memory. Open starts a listing at its first
  // page; Append and Prepend extend the window by one page at either end.
  enum class PageLoad : uint8_t { Open, Append, Prepend };

  // A minimal network boot never brought up the rest of the app, so leaving the
  // catalog from one restarts even when Wi-Fi never came up.
  const bool networkBoot;
  const bool resumeDownload;
  // Where the BookOrbit Home sent this session; consumed once Wi-Fi is up.
  BookOrbitCatalogLink entryLink;
  PendingDownload pendingDownload;
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  // The rows in memory: a whole root or on-device list, or a window of a few
  // consecutive server pages of a book or facet listing. Every scroll position
  // (selectorIndex, topIndex) counts rows from the start of the listing, so a page
  // dropped from the window changes no index; entries[0] is row windowStart.
  std::vector<Entry> entries;
  int windowStart = 0;
  int selectorIndex = 0;
  NavLevel navLevel = NavLevel::Root;
  bool consumeConfirm = false;
  // Set once a Confirm hold has opened the book action menu; input is swallowed
  // until Confirm is up again so its release does not also activate the row.
  bool longPressFired = false;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool goHomeAfterCancel = false;
  // The download was started from a BookOrbit Home shelf: whatever its outcome, it ends
  // on Home rather than on the listing it was fetched through.
  bool downloadFromHome = false;

  // Current book-list context, used to page and to return to the same list after a download.
  BookOrbitBookQuery listQuery;
  // The read-status filter the listing opened with ("reading" for Continue reading): the
  // header names the filter only once the user picked another.
  std::string listDefaultReadStatus;
  std::string listTitle;  // without a count: the header adds listTotal
  int listPage = 1;
  int listTotal = 0;
  int listPageSize = BookOrbitCatalogClient::PAGE_SIZE;
  // Set when the current book list was opened from a facet entry, so Back returns
  // to the facet listing instead of the root.
  bool booksFromFacet = false;
  // The book list is an on-device one (On device, In progress): no server query behind
  // it, so nothing to sort or filter.
  bool listIsLocal = false;
  // Set while the listing is freed for a download's heap headroom, cleared once a
  // listing loads again. Distinguishes "rebuild the list Back should return to"
  // from a listing that genuinely loaded empty, where Back must navigate up
  // instead of reloading the same empty listing forever.
  bool listFreedForDownload = false;

  // Current facet-list context (authors or series).
  std::string facetSectionId;
  std::string facetTitle;
  int facetPage = 1;
  bool facetHasNext = false;
  // The facet row a book list was opened from, which Back returns to.
  int facetSelectorIndex = 0;
  int facetTopIndex = 0;
  // Book counts of the on-device root entries, as last counted for their badges: what
  // opening one reserves rows for.
  size_t localOnDeviceCount = 0;
  size_t localInProgressCount = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  // The sort or filter picker while it is open. Allocated for the pick and freed
  // right after, before the listing it changes is fetched: its strings and layout
  // must not sit in the heap the TLS session is about to need.
  std::unique_ptr<OptionPopup> listOptions;
  ListOption openListOption = ListOption::Sort;
  int pendingListChoice = -1;  // the row picked, applied once the picker has closed

  // Where each letter starts in a listing sorted by title or author, from its first page;
  // none for any other listing. The heading strings stay put for the list to point at.
  struct LetterStart {
    int row = 0;
    char heading[2] = {'#', '\0'};
  };
  static constexpr size_t MAX_LETTERS = 27;  // A to Z, and '#'
  std::array<LetterStart, MAX_LETTERS> listLetters{};
  size_t listLetterCount = 0;

  // Section headings take room, so a screen holds fewer rows than visibleRows assumes.
  // The render task measures how many it drew and, when the selection or the listing's
  // end fell below the screen, draws again further down. The loop keeps owning topIndex:
  // it takes the render's correction only for the very top and selection it rendered
  // from (adoptRenderedViewport), never mid-walk.
  static constexpr int MAX_RENDER_PASSES = 4;
  freeink::ui::ListNav listMeasure;  // render task: measures rows drawn, never scrolls
  int renderPass = 0;                // render task: 0 for a frame's first layout
  int renderTop = 0;                 // render task: the row the frame is drawn from
  bool renderPassAgain = false;      // render task: the frame needs a pass from renderTop
  int renderFromTop = 0;             // render task: topIndex and selectorIndex as the
  int renderFromSelection = 0;       //   frame started, which its correction answers
  uint32_t listingGeneration = 0;    // bumped by startListing(), under the render lock
  std::atomic<uint32_t> renderedListing{0};
  std::atomic<int> renderedFromTop{-1};
  std::atomic<int> renderedFromSelection{-1};
  std::atomic<int> renderedTop{0};
  std::atomic<int> renderedRows{0};

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onControlEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void activateSelected();
  // Opens a server book listing from a root section, a facet entry or a search.
  void openBookListing(const BookOrbitBookQuery& query, const std::string& title, bool fromFacet);
  bool listOptionsAvailable() const;
  // The Library's list view setting: Expanded heads the list's sections, as it does there.
  bool sectionsShown() const;
  // The heading drawn above the row at entries[index], or nullptr.
  const char* sectionHeading(size_t index) const;
  void setListLetters(const std::vector<BookOrbitLetterBucket>& letters);
  // Takes the top the last frame was really drawn from, when it was drawn for the
  // current top and selection (see listMeasure).
  void adoptRenderedViewport();
  // Rows a screen holds from topIndex: what the last frame drew, when sections made that
  // fewer than visibleRows.
  int screenRows() const;
  void showListOptions(ListOption option);
  // Runs the picker; true while it holds the input.
  bool handleListOptions();
  void applyListChoice(ListOption option, int choice);
  void reloadListing(const BookOrbitBookQuery& query);
  std::string headerTitle() const;
  void openLocalBook(const std::string& path);
  void showBookActionMenu(bool ignoreInitialConfirmRelease);
  void promptDeleteBook(int row);
  void navigateBack();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  bool loadRoot(bool allowNetwork = true);
  // By value: callers pass a row's own section id, and the listing it opens replaces that row.
  void loadLocalBooks(std::string kind);
  size_t collectLocalBooks(const std::string& kind, std::vector<Entry>* sink);
  bool loadFacetEntries(const std::string& sectionId, const std::string& title, int page,
                        PageLoad load = PageLoad::Open, bool allowNetwork = true);
  bool loadBooks(const BookOrbitBookQuery& query, const std::string& title, int page, bool fromFacet,
                 PageLoad load = PageLoad::Open, bool allowNetwork = true);

  // Row window over the current listing (see `entries`).
  int loadedEnd() const { return windowStart + static_cast<int>(entries.size()); }
  const Entry* entryAt(int row) const;
  Entry* entryAt(int row);
  bool hasMorePages() const;
  // Empties the rows for a new listing that will hold about `rows`, giving back a
  // larger block a previous on-device listing left behind.
  void startListing(size_t rows);
  // Room for `count` blank rows of server page `page`; returns the index of the first.
  size_t placePageRows(PageLoad load, size_t count, uint16_t page);
  // Drop whole pages from one end of the window while it holds more than
  // MAX_WINDOW_PAGES (counting `incoming` pages about to join it) and the page lies
  // wholly outside the rows to keep.
  void dropPagesBefore(int keepFrom, int incoming = 0);
  void dropPagesAfter(int keepThrough, int incoming = 0);
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
  // Opens the current book or facet listing again down to `lastPage`, walking its
  // pages in order so row numbers stay exact, then restores the scroll position.
  bool reopenListing(NavLevel level, int lastPage, int wantedSelector, int wantedTop);
  void restoreBookListAfterDownload();
  void launchSearch();
  void performSearch(const std::string& query);
  // replacePath: re-download over that existing on-device file instead of the
  // server's path; the old copy is only swapped out once the new one is complete.
  void downloadBook(int64_t bookId, const std::string& title, const std::string& replacePath = "");
  void showDownloadScreen(const std::string& title);
  // Saves the download and the listing, then restarts into the BOOKORBIT_DOWNLOAD
  // boot. False when this device downloads in place instead.
  bool restartForDownload(int64_t bookId, int64_t fileId, const std::string& title, const std::string& path);
  void runPendingDownload(bool wifiConnected);
  // The transfer itself, then the listing it freed is rebuilt.
  void transferBook(int64_t bookId, int64_t fileId, const std::string& finalPath);
  bool preventAutoSleep() override;
};
