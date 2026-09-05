#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "network/BookOrbitCatalogClient.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from BookOrbit's catalog.
 *
 * Simplified compared to BookOrbit's own KOReader plugin: three navigation levels
 * (root sections -> optional authors/series/collections facet list -> a paged book
 * list or a search). Library/smart-scope drill-down, covers, ratings and
 * read-status editing remain out of scope (see SCOPE.md discussion for BookOrbit
 * sync).
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
    std::string subtitle;   // author, for BOOK/LOCAL_BOOK entries
    std::string sectionId;  // section id (SECTION/FACET_SECTION/LOCAL_SECTION) or facet entry id (FACET)
    std::string seriesId;   // numeric series id, for FACET entries of the series facet
    std::string path;       // SD path, for LOCAL_BOOK entries (opened in the reader)
    int64_t bookId = 0;
    bool onDevice = false;  // BOOK entries: a matching file already exists on the device
  };

  explicit BookOrbitCatalogBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // FreeInkApp hosts the entry list (themed rows, touch routing); the compact
  // header keeps its own painter so the catalog's chrome stays as it was.
  // 24 interaction slots cover the densest page at the smallest UI scale.
  using UiApp = freeink::ui::FreeInkApp<24, 4>;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<Entry> entries;
  int selectorIndex = 0;
  NavLevel navLevel = NavLevel::Root;
  bool consumeConfirm = false;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool goHomeAfterCancel = false;

  // Current book-list context, used to page and to return to the same list after a download.
  BookOrbitBookQuery listQuery;
  std::string listTitle;
  int listPage = 1;
  int listTotal = 0;
  int listPageSize = BookOrbitCatalogClient::PAGE_SIZE;
  // Set when the current book list was opened from a facet entry, so Back returns
  // to the facet listing instead of the root.
  bool booksFromFacet = false;

  // Current facet-list context (authors or series).
  std::string facetSectionId;
  std::string facetTitle;
  int facetPage = 1;
  bool facetHasNext = false;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void activateSelected();
  void navigateBack();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  bool loadRoot(bool allowNetwork = true);
  void loadLocalBooks(const std::string& kind);
  bool loadFacetEntries(const std::string& sectionId, const std::string& title, int page, bool append = false,
                        bool allowNetwork = true);
  bool loadBooks(const BookOrbitBookQuery& query, const std::string& title, int page, bool fromFacet,
                 bool append = false, bool allowNetwork = true);
  bool appendNextPageForCurrentList(bool allowNetwork = true);
  void restoreBookListAfterDownload();
  void launchSearch();
  void performSearch(const std::string& query);
  void downloadBook(int64_t bookId, const std::string& title);
  bool preventAutoSleep() override;
};
