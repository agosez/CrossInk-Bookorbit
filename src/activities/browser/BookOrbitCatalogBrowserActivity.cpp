#include "BookOrbitCatalogBrowserActivity.h"

#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

#include "./BookOrbitCatalogListCache.h"
#include "AppVersion.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CompactHeader.h"
#include "components/HeaderDate.h"
#include "components/OptionPopup.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookOrbitIcons.h"
#include "components/icons/libraryIcons.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/BookOrbitDevicePath.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
using Entry = BookOrbitCatalogBrowserActivity::Entry;

constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_CONTROL = 2;
// A server book list's header controls on touch devices, sized and spaced like the Library's.
constexpr int HEADER_CONTROL_SIZE = 44;
constexpr int HEADER_CONTROL_GAP = 10;
constexpr int16_t HEADER_CONTROL_SORT = 0;
constexpr int16_t HEADER_CONTROL_FILTER = 1;

// The Library's sort methods that BookOrbit can apply, in the Library's order. The server
// sorts authors by last name and has no first-name or genre order.
struct SortChoice {
  const char* id;  // BookOrbit's sort id
  StrId label;
  bool descendingFirst;  // the server's own direction for it: newest first for dates
};
constexpr SortChoice SORT_CHOICES[] = {
    {"recently_added", StrId::STR_LIBRARY_DATE_ADDED, true},
    {"title", StrId::STR_LIBRARY_TITLE, false},
    {"author", StrId::STR_LIBRARY_AUTHOR_LAST_NAME, false},
    {"recently_read", StrId::STR_BOOKORBIT_SORT_RECENTLY_READ, true},
    {"series", StrId::STR_LIBRARY_SERIES, false},
};
constexpr int SORT_CHOICE_COUNT = static_cast<int>(std::size(SORT_CHOICES));
// The sort picker's rows: the direction and the list view above a divider, then the methods.
constexpr int SORT_ROW_DIRECTION = 0;
constexpr int SORT_ROW_LIST_VIEW = 1;
constexpr int SORT_ROW_FIRST_METHOD = 2;
// What the server sorts by when a query names no sort (an author's books).
constexpr int DEFAULT_SORT_CHOICE = 0;

struct ReadStatusChoice {
  const char* id;  // BookOrbit's readStatus filter; empty for every book
  StrId label;
};
constexpr ReadStatusChoice READ_STATUS_CHOICES[] = {
    {"", StrId::STR_BOOKORBIT_STATUS_ALL},
    {"unread", StrId::STR_BOOKORBIT_STATUS_UNREAD},
    {"reading", StrId::STR_BOOKORBIT_STATUS_READING},
    {"finished", StrId::STR_BOOKORBIT_STATUS_FINISHED},
};
constexpr int READ_STATUS_CHOICE_COUNT = static_cast<int>(std::size(READ_STATUS_CHOICES));

int sortChoiceIndex(const std::string& sort) {
  if (sort.empty()) return DEFAULT_SORT_CHOICE;
  for (int i = 0; i < SORT_CHOICE_COUNT; i++) {
    if (sort == SORT_CHOICES[i].id) return i;
  }
  return DEFAULT_SORT_CHOICE;
}

bool sortsDescending(const BookOrbitBookQuery& query) {
  if (!query.order.empty()) return query.order == "desc";
  return SORT_CHOICES[sortChoiceIndex(query.sort)].descendingFirst;
}

int readStatusChoiceIndex(const std::string& readStatus) {
  for (int i = 0; i < READ_STATUS_CHOICE_COUNT; i++) {
    if (readStatus == READ_STATUS_CHOICES[i].id) return i;
  }
  return 0;
}

// A row's title without the count badge the root and facet listings give it.
std::string titleWithoutBadge(const Entry& entry) {
  return entry.badgeAt > 0 ? entry.title.substr(0, entry.badgeAt) : entry.title;
}

// A server book list's header controls on touch devices: the Library's, minus its refresh
// and search. Sort, whose arrow shows the direction, then filter, right-aligned clear of
// the battery. A template, so a button-only build, whose call site is dead, never emits it.
template <typename Screen>
void addListHeaderControls(Screen& screen, const GfxRenderer& renderer, const bool descending) {
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

// Appends a row's " (count)" badge, remembering where the title itself ends.
void addCountBadge(Entry& entry, const int count) {
  entry.badgeAt = static_cast<uint16_t>(entry.title.size());
  entry.title += " (" + std::to_string(count) + ")";
}

constexpr size_t BOOKORBIT_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr char READ_FOLDER_PREFIX[] = "/Read";
constexpr size_t MAX_LOCAL_ENTRIES = 200;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 1000;
constexpr unsigned long DOWNLOAD_PROGRESS_MAX_UPDATE_MS = 5000;
// Hold threshold for the book action menu (firmware convention, as in Recent Books).
constexpr unsigned long LONG_PRESS_MS = 1000;
// Where HttpDownloader stages a download until it is complete and checked (see
// DownloadFileSwap). Not ".epub", so a leftover never shows up as a book.
constexpr char STAGED_DOWNLOAD_SUFFIX[] = ".part";
// Marker appended (right-aligned) to catalog rows whose book already exists on the
// device. U+2022 bullet: guaranteed by the built-in fonts' default glyph intervals.
constexpr char ON_DEVICE_MARKER[] = "\xE2\x80\xA2";
// Server pages a book or facet listing keeps in memory. A listing used to keep every
// page it had shown, about 150 bytes a row plus its strings, until a C3 catalog session
// had no block left for a 16 KB TLS record by the third page (issue #96). Two pages
// cover a screen and the next; a third arrives before the first one goes.
constexpr int MAX_WINDOW_PAGES = 2;
// Reserved once for the session, so the rows never move through doubling reallocations
// that would leave holes next to the TLS buffers.
constexpr size_t WINDOW_ROWS = (MAX_WINDOW_PAGES + 1) * BookOrbitCatalogClient::PAGE_SIZE;

// The SD filename (leading slash, no folder) this firmware falls back to when the
// server does not say where the file belongs.
std::string catalogBookFilename(const std::string& title, const std::string& author) {
  const std::string suffix = author.empty() ? "" : (" - " + author);
  return "/" + StringUtils::sanitizeFilename(title + suffix) + ".epub";
}

// Where a catalog file belongs under the download folder, leading slash included.
// BookOrbit resolves the account's KOReader file naming template (or this device's
// override) and returns it as the file's devicePath, so a book lands where the same
// account's KOReader devices put it — series folders and all. Servers that do not
// send one keep the title-author name this fork always used.
std::string catalogBookFilename(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file) {
  const std::string fromServer = BookOrbitDevicePath::sanitizeRelativePath(file.devicePath);
  if (!fromServer.empty()) return "/" + fromServer;
  return catalogBookFilename(detail.title, detail.author);
}

// The full SD path a catalog book downloads to: the configured download folder
// ("" = SD root) plus the filename above.
std::string catalogBookPath(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file) {
  return BOOKORBIT_STORE.getDownloadFolder() + catalogBookFilename(detail, file);
}

// True when the catalog book already exists locally, with its path in outPath. The download index knows
// where past downloads landed, whatever the folder setting or the server's naming
// template was at the time; the fallback filename heuristic covers pre-index
// downloads in the current download location, the SD root (the old fixed location)
// and the /Read folder the finished-book move feature uses. It cannot cover the
// server's naming template: only the book detail carries the resolved path, and a
// listing would need one request per row to know it. Books downloaded under a
// template but missing from the index (another device, or an index dropped when the
// server URL changed) are therefore not detected — as with books moved or renamed by
// hand, this stays a best-effort convenience marker.
bool bookOnDevice(const int64_t bookId, const std::string& title, const std::string& author, std::string& outPath) {
  if (BookOrbitDownloadIndex::lookup(bookId, outPath)) return true;
  const std::string filename = catalogBookFilename(title, author);
  const std::string& folder = BOOKORBIT_STORE.getDownloadFolder();
  // Candidates are built aside: outPath is a listed row's own path, and most catalog
  // books are not on the device, so a miss must not leave a path buffer in the row.
  const auto found = [&outPath](std::string candidate) {
    if (!Storage.exists(candidate.c_str())) return false;
    outPath = std::move(candidate);
    return true;
  };
  if (!folder.empty() && found(folder + filename)) return true;
  if (found(filename) || found(READ_FOLDER_PREFIX + filename)) return true;
  std::string().swap(outPath);  // re-checked after a delete: release the old path too
  return false;
}

std::string bookTitleFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t end = path.size() > start + 5 ? path.size() - 5 : path.size();  // strip ".epub"
  return path.substr(start, end - start);
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

BookOrbitCatalogBrowserActivity::BookOrbitCatalogBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                                 const bool networkBoot, const bool resumeDownload)
    : Activity("BookOrbitCatalogBrowser", renderer, mappedInput),
      networkBoot(networkBoot || resumeDownload),
      resumeDownload(resumeDownload),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

BookOrbitCatalogBrowserActivity::~BookOrbitCatalogBrowserActivity() = default;

void BookOrbitCatalogBrowserActivity::onControlEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitCatalogBrowserActivity*>(user);
  self->app.clearTapFlash();
  if (!self->listOptionsAvailable()) return;
  self->showListOptions(event.value == HEADER_CONTROL_FILTER ? ListOption::Filter : ListOption::Sort);
}

void BookOrbitCatalogBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitCatalogBrowserActivity*>(user);
  // The list was given the rows in memory, so its action values index `entries`.
  const int row = self->windowStart + event.value;
  const Entry* entry = self->entryAt(row);
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

  entries.clear();
  windowStart = 0;
  selectorIndex = 0;
  navLevel = NavLevel::Root;
  listIsLocal = false;
  listDefaultReadStatus.clear();
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
    navLevel = NavLevel::Books;
    listQuery = std::move(resume.listQuery);
    listDefaultReadStatus = std::move(resume.listDefaultReadStatus);
    listTitle = std::move(resume.listTitle);
    listPage = resume.listPage;
    booksFromFacet = resume.booksFromFacet;
    selectorIndex = resume.selectorIndex;
    topIndex = resume.topIndex;
    facetSectionId = std::move(resume.facetSectionId);
    facetTitle = std::move(resume.facetTitle);
    facetPage = resume.facetPage;
    facetSelectorIndex = resume.facetSelectorIndex;
    facetTopIndex = resume.facetTopIndex;
    pendingDownload = {resume.bookId, resume.fileId, std::move(resume.title), std::move(resume.path)};
  } else {
    BookOrbitCatalogListCache::clear();
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
  entries.clear();
  BookOrbitDownloadIndex::unload();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  } else if (networkBoot) {
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
  if (pendingDownload.fileId != 0) {
    runPendingDownload(connected);
    return;
  }
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

  // Per-section entry counts, the way BookOrbit's own plugin badges its Browse
  // tiles: the dashboard plus the totals of the two EPUB book listings, cached
  // with the listings. Non-fatal — an older server without the dashboard just
  // shows the sections without counts.
  BookOrbitCatalogCounts counts;
  if (!BookOrbitCatalogListCache::loadCatalogCounts(counts) && allowNetwork &&
      BookOrbitCatalogClient::fetchCatalogCounts(counts)) {
    BookOrbitCatalogListCache::saveCatalogCounts(counts);
  }
  const auto sectionCount = [&counts](const std::string& id) {
    if (id == "all-books" || id == "recent") return counts.totalBooks;
    if (id == "continue-reading") return counts.inProgress;
    if (id == "libraries") return counts.libraries;
    if (id == "authors") return counts.authors;
    if (id == "series") return counts.series;
    if (id == "collections") return counts.collections;
    if (id == "smart-scopes") return counts.smartScopes;
    return -1;
  };

  // Local, offline categories: what's already on the SD card, counted on the spot.
  localOnDeviceCount = collectLocalBooks("on-device", nullptr);
  localInProgressCount = collectLocalBooks("in-progress", nullptr);

  RenderLock lock(*this);             // the render task draws the rows replaced here
  startListing(sections.size() + 4);  // the server's sections, Want to read, the two local ones, Search
  listFreedForDownload = false;
  for (auto& section : sections) {
    Entry entry;
    const bool isFacet = section.id == "authors" || section.id == "series" || section.id == "collections" ||
                         section.id == "libraries" || section.id == "smart-scopes";
    entry.type = isFacet ? EntryType::FACET_SECTION : EntryType::SECTION;
    entry.title = section.title;
    const int count = sectionCount(section.id);
    if (count >= 0) addCountBadge(entry, count);
    entry.sectionId = section.id;
    entries.push_back(std::move(entry));
  }
  // The want-to-read shelf BookOrbit's own plugin shows, which the server does not list
  // among its sections. After them, so the rows above keep their places.
  if (counts.wantToRead >= 0) {
    Entry wantToRead;
    wantToRead.type = EntryType::SECTION;
    wantToRead.title = tr(STR_BOOKORBIT_WANT_TO_READ);
    addCountBadge(wantToRead, counts.wantToRead);
    wantToRead.sectionId = BookOrbitCatalogClient::WANT_TO_READ_SHELF;
    entries.push_back(std::move(wantToRead));
  }
  Entry onDevice;
  onDevice.type = EntryType::LOCAL_SECTION;
  onDevice.title = tr(STR_BOOKORBIT_ON_DEVICE);
  addCountBadge(onDevice, static_cast<int>(localOnDeviceCount));
  onDevice.sectionId = "on-device";
  entries.push_back(std::move(onDevice));
  Entry inProgress;
  inProgress.type = EntryType::LOCAL_SECTION;
  inProgress.title = tr(STR_BOOKORBIT_IN_PROGRESS);
  addCountBadge(inProgress, static_cast<int>(localInProgressCount));
  inProgress.sectionId = "in-progress";
  entries.push_back(std::move(inProgress));
  Entry search;
  search.type = EntryType::SEARCH;
  search.title = tr(STR_SEARCH);
  entries.push_back(std::move(search));

  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  lock.unlock();
  requestUpdate();
  return true;
}

// Enumerate the local books behind one of the offline root categories, appending
// Entry rows when a sink is given. Returns the count either way, so the root can
// badge the categories without building (and then discarding) their rows.
size_t BookOrbitCatalogBrowserActivity::collectLocalBooks(const std::string& kind, std::vector<Entry>* sink) {
  size_t count = 0;
  if (kind == "in-progress") {
    // Books with local reading progress: the recent-books list minus finished ones.
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (!FsHelpers::hasEpubExtension(book.path) || !Storage.exists(book.path.c_str())) continue;
      const BookReadingStats stats = BookReadingStats::load(Epub::cachePathForFilePath(book.path, "/.crosspoint"));
      if (stats.isCompleted) continue;
      count++;
      if (!sink) continue;
      Entry entry;
      entry.type = EntryType::LOCAL_BOOK;
      entry.title = book.title.empty() ? book.path : book.title;
      entry.subtitle = book.author;
      entry.path = book.path;
      sink->push_back(std::move(entry));
    }
    LOG_DBG("BookOrbit", "Local scan: kind=%s count=%u", kind.c_str(), static_cast<unsigned>(count));
    return count;
  }

  // This firmware's own catalog downloads come from the index, which recorded where
  // each one landed. That is what makes the server's file naming template a non-issue
  // here: however deep it filed a book, the path is known and nothing has to be
  // walked. A download whose index entry is gone (another device, or an index
  // discarded when the server URL changed) falls back to the directory scan below,
  // and is missed when the template filed it in a subfolder.
  struct LocalBookCollector {
    size_t* count;
    std::vector<Entry>* sink;
  };
  LocalBookCollector collector{&count, sink};
  BookOrbitDownloadIndex::forEachExisting(
      [](const int64_t, const std::string& path, void* context) {
        auto* out = static_cast<LocalBookCollector*>(context);
        if (*out->count >= MAX_LOCAL_ENTRIES) return;
        (*out->count)++;
        if (out->sink == nullptr) return;
        Entry entry;
        entry.type = EntryType::LOCAL_BOOK;
        entry.title = bookTitleFromPath(path);
        entry.path = path;
        out->sink->push_back(std::move(entry));
      },
      &collector);

  // Then every other EPUB sitting in the places books arrive: the configured download
  // folder, the SD root (where older firmware downloaded, and where books copied over
  // USB land) and the /Read folder the finished-book move uses. Flat on purpose —
  // descending would walk every folder on the card to find books that, if they came
  // from the catalog, the index already listed.
  const auto scanDir = [&count, sink](const std::string& dirPath) {
    FsFile dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      dir.close();
      return;
    }
    const std::string prefix = dirPath == "/" ? dirPath : dirPath + "/";
    char name[128];
    std::string path;  // reused: one allocation for the whole directory, not per file
    FsFile file;
    while (count < MAX_LOCAL_ENTRIES && (file = dir.openNextFile())) {
      const size_t nameLen = file.isDirectory() ? 0 : file.getName(name, sizeof(name));
      file.close();
      if (nameLen <= 5 || !FsHelpers::hasEpubExtension(std::string_view(name, nameLen))) continue;
      path.assign(prefix).append(name, nameLen);
      if (BookOrbitDownloadIndex::hasPath(path)) continue;  // already listed from the index
      count++;
      if (!sink) continue;
      Entry entry;
      entry.type = EntryType::LOCAL_BOOK;
      entry.title = std::string(name, nameLen - 5);  // strip ".epub"
      entry.path = path;
      sink->push_back(std::move(entry));
    }
    dir.close();
  };
  const std::string& folder = BOOKORBIT_STORE.getDownloadFolder();
  if (!folder.empty() && folder != READ_FOLDER_PREFIX) scanDir(folder);
  scanDir("/");
  scanDir(READ_FOLDER_PREFIX);
  LOG_DBG("BookOrbit", "Local scan: kind=%s count=%u", kind.c_str(), static_cast<unsigned>(count));
  return count;
}

void BookOrbitCatalogBrowserActivity::loadLocalBooks(const std::string kind) {
  const bool inProgress = kind == "in-progress";
  RenderLock lock(*this);  // the render task draws the rows replaced here
  listTitle = inProgress ? tr(STR_BOOKORBIT_IN_PROGRESS) : tr(STR_BOOKORBIT_ON_DEVICE);
  // Sized by the root's badge count: this list is sorted whole, so it cannot be paged
  // like a server listing, and growing it a row at a time would double its block.
  startListing(inProgress ? localInProgressCount : localOnDeviceCount);

  collectLocalBooks(kind, &entries);
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (FsHelpers::naturalLess(a.title, b.title)) return true;
    if (FsHelpers::naturalLess(b.title, a.title)) return false;
    return FsHelpers::naturalLess(a.subtitle, b.subtitle);
  });

  // Local listings behave like a book list one level below the root. Neutralise
  // the paging context left by a previous server listing: without this, reaching
  // the bottom of a local list could append SERVER results into it, and the
  // scroll indicator would size itself on the stale server total.
  navLevel = NavLevel::Books;
  booksFromFacet = false;
  listIsLocal = true;
  listFreedForDownload = false;
  listPage = 1;
  listTotal = static_cast<int>(entries.size());
  listPageSize = std::max<int>(1, static_cast<int>(entries.size()));
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  lock.unlock();
  requestUpdate();
}

bool BookOrbitCatalogBrowserActivity::loadFacetEntries(const std::string& sectionId, const std::string& title,
                                                       const int page, const PageLoad load, const bool allowNetwork) {
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
  // the currently displayed list and its paging state consistent. The arguments can
  // be the rows' own strings, so they are copied before the rows change. The render
  // task draws these rows, and the window's inserts and erases move them: hold it off.
  RenderLock lock(*this);
  navLevel = NavLevel::FacetList;
  listFreedForDownload = false;
  facetSectionId = sectionId;
  facetTitle = title;
  // The window's last page says whether more follow; a page put in front changes neither.
  if (load != PageLoad::Prepend) {
    facetPage = page;
    facetHasNext = result.hasNext;
  }

  if (load == PageLoad::Open) startListing(result.entries.size());
  // Make room before the page arrives, so the window stays within its reserve: trimmed
  // after, it briefly holds one page too many and the vector moves to a larger block.
  if (load == PageLoad::Append) dropPagesBefore(topIndex, /*incoming=*/1);
  if (load == PageLoad::Prepend) dropPagesAfter(topIndex + visibleRows - 1, /*incoming=*/1);
  // Server order is kept as-is: it is already alphabetical for facets, and a
  // per-page sort would break the overall order once pages are appended.
  const size_t first = placePageRows(load, result.entries.size(), static_cast<uint16_t>(page));
  for (size_t i = 0; i < result.entries.size(); i++) {
    auto& facet = result.entries[i];
    Entry& entry = entries[first + i];
    entry.type = EntryType::FACET;
    entry.title = std::move(facet.title);
    if (facet.count > 0) addCountBadge(entry, facet.count);
    entry.sectionId = std::move(facet.id);
    entry.seriesId = std::move(facet.seriesId);
  }
  logPageLoad(page, cacheHit);
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  lock.unlock();
  requestUpdate();
  return true;
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
  RenderLock lock(*this);
  navLevel = NavLevel::Books;
  listFreedForDownload = false;
  listQuery = query;
  listTitle = title;
  if (load != PageLoad::Prepend) listPage = page;
  booksFromFacet = fromFacet;
  listIsLocal = false;

  listTotal = result.total;
  listPageSize = result.pageSize > 0 ? result.pageSize : BookOrbitCatalogClient::PAGE_SIZE;

  if (load == PageLoad::Open) startListing(result.books.size());
  // Make room before the page arrives, so the window stays within its reserve: trimmed
  // after, it briefly holds one page too many and the vector moves to a larger block.
  if (load == PageLoad::Append) dropPagesBefore(topIndex, /*incoming=*/1);
  if (load == PageLoad::Prepend) dropPagesAfter(topIndex + visibleRows - 1, /*incoming=*/1);
  // Server order carries the listing's meaning -- recency for "Continue reading"
  // and "Recently added", series order for series -- and appending pages keeps
  // it consistent; a client-side re-sort would destroy both.
  const size_t first = placePageRows(load, result.books.size(), static_cast<uint16_t>(page));
  for (size_t i = 0; i < result.books.size(); i++) {
    auto& book = result.books[i];
    Entry& entry = entries[first + i];
    entry.type = EntryType::BOOK;
    entry.bookId = book.id;
    entry.onDevice = bookOnDevice(book.id, book.title, book.author, entry.path);
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
  // The letters span the whole listing and only come with its first page.
  if (page == 1) setListLetters(result.letters);
  logPageLoad(page, cacheHit);
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  lock.unlock();
  requestUpdate();
  return true;
}

const BookOrbitCatalogBrowserActivity::Entry* BookOrbitCatalogBrowserActivity::entryAt(const int row) const {
  const int index = row - windowStart;
  if (index < 0 || index >= static_cast<int>(entries.size())) return nullptr;
  return &entries[index];
}

BookOrbitCatalogBrowserActivity::Entry* BookOrbitCatalogBrowserActivity::entryAt(const int row) {
  return const_cast<Entry*>(std::as_const(*this).entryAt(row));
}

bool BookOrbitCatalogBrowserActivity::hasMorePages() const {
  if (navLevel == NavLevel::FacetList) return facetHasNext;
  if (navLevel == NavLevel::Books) return static_cast<long>(listPage) * listPageSize < listTotal;
  return false;
}

void BookOrbitCatalogBrowserActivity::startListing(const size_t rows) {
  const size_t wanted = std::max(rows, WINDOW_ROWS);
  if (entries.capacity() > wanted) {
    // Free the large block first, so the smaller one can be carved out of it.
    std::vector<Entry>().swap(entries);
  }
  entries.clear();
  entries.reserve(wanted);
  windowStart = 0;
  selectorIndex = 0;
  topIndex = 0;
  listLetterCount = 0;
  // What the render task measured belongs to the previous listing.
  listingGeneration++;
}

void BookOrbitCatalogBrowserActivity::setListLetters(const std::vector<BookOrbitLetterBucket>& letters) {
  listLetterCount = 0;
  for (const auto& letter : letters) {
    if (listLetterCount == MAX_LETTERS) break;
    LetterStart& start = listLetters[listLetterCount++];
    start.row = letter.row;
    start.heading[0] = letter.letter;
  }
}

bool BookOrbitCatalogBrowserActivity::sectionsShown() const {
  return SETTINGS.libraryListExpanded != 0 && navLevel == NavLevel::Books && !listIsLocal;
}

const char* BookOrbitCatalogBrowserActivity::sectionHeading(const size_t index) const {
  if (listLetterCount > 0) {
    const int row = windowStart + static_cast<int>(index);
    for (size_t i = 0; i < listLetterCount; i++) {
      if (listLetters[i].row == row) return listLetters[i].heading;
    }
    return nullptr;
  }
  const Entry& entry = entries[index];
  if (entry.type != EntryType::BOOK || entry.sectionId.empty()) return nullptr;
  // The first row in memory may go on with a section that began on a page no longer held:
  // it is headed again, which names the section the screen opens on.
  if (index > 0 && entries[index - 1].sectionId == entry.sectionId) return nullptr;
  return entry.sectionId.c_str();
}

void BookOrbitCatalogBrowserActivity::adoptRenderedViewport() {
  if (renderedListing.load(std::memory_order_acquire) != listingGeneration) return;
  if (renderedFromTop.load() != topIndex || renderedFromSelection.load() != selectorIndex) return;
  const int top = renderedTop.load();
  if (top >= windowStart && top < loadedEnd()) topIndex = top;
}

int BookOrbitCatalogBrowserActivity::screenRows() const {
  const int rows = renderedRows.load();
  if (renderedListing.load(std::memory_order_acquire) != listingGeneration || renderedTop.load() != topIndex ||
      rows <= 0) {
    return visibleRows;
  }
  return std::min(rows, visibleRows);
}

size_t BookOrbitCatalogBrowserActivity::placePageRows(const PageLoad load, const size_t count, const uint16_t page) {
  // An exact reserve: insert() would otherwise double the block. Only a listing whose
  // pages or screen are larger than WINDOW_ROWS planned for gets here.
  if (entries.size() + count > entries.capacity()) entries.reserve(entries.size() + count);
  Entry blank;
  blank.type = EntryType::BOOK;
  blank.page = page;
  if (load == PageLoad::Prepend) {
    entries.insert(entries.begin(), count, blank);
    windowStart -= static_cast<int>(count);
    return 0;
  }
  const size_t first = entries.size();
  entries.insert(entries.end(), count, blank);
  return first;
}

void BookOrbitCatalogBrowserActivity::dropPagesBefore(const int keepFrom, const int incoming) {
  while (!entries.empty() && entries.back().page - entries.front().page + 1 + incoming > MAX_WINDOW_PAGES) {
    const uint16_t page = entries.front().page;
    const auto end =
        std::find_if(entries.begin(), entries.end(), [page](const Entry& entry) { return entry.page != page; });
    const int count = static_cast<int>(end - entries.begin());
    if (windowStart + count > keepFrom) break;  // the viewport still shows that page
    entries.erase(entries.begin(), end);
    windowStart += count;
  }
  clampSelectionToWindow();
}

void BookOrbitCatalogBrowserActivity::dropPagesAfter(const int keepThrough, const int incoming) {
  while (!entries.empty() && entries.back().page - entries.front().page + 1 + incoming > MAX_WINDOW_PAGES) {
    const uint16_t page = entries.back().page;
    const auto start = std::find_if(entries.rbegin(), entries.rend(), [page](const Entry& entry) {
                         return entry.page != page;
                       }).base();
    if (windowStart + static_cast<int>(start - entries.begin()) <= keepThrough) break;
    entries.erase(start, entries.end());
    // The window now ends a page earlier, and the server still has that page after it.
    if (navLevel == NavLevel::FacetList) {
      facetPage = page - 1;
      facetHasNext = true;
    } else {
      listPage = page - 1;
    }
  }
  clampSelectionToWindow();
}

void BookOrbitCatalogBrowserActivity::logPageLoad(const int page, const bool cacheHit) const {
  LOG_INF("BookOrbit", "Listing page %d from %s: %u rows in memory from row %d (free=%u maxAlloc=%u)", page,
          cacheHit ? "cache" : "server", static_cast<unsigned>(entries.size()), windowStart, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
}

// A swipe leaves the selection where it was while the viewport moves on, so a dropped
// page can take the selected row with it; the selection then holds the nearest row.
void BookOrbitCatalogBrowserActivity::clampSelectionToWindow() {
  if (entries.empty()) return;
  selectorIndex = std::clamp(selectorIndex, windowStart, loadedEnd() - 1);
}

bool BookOrbitCatalogBrowserActivity::appendNextPageForCurrentList(const bool allowNetwork) {
  const int previousEnd = loadedEnd();
  if (navLevel == NavLevel::FacetList && facetHasNext) {
    if (!loadFacetEntries(facetSectionId, facetTitle, facetPage + 1, PageLoad::Append, /*allowNetwork=*/false)) {
      if (!allowNetwork) return false;
      showLoadingBeforeFetch();
      loadFacetEntries(facetSectionId, facetTitle, facetPage + 1, PageLoad::Append);
    }
    return state == BrowserState::BROWSING && loadedEnd() > previousEnd;
  }

  if (navLevel == NavLevel::Books && hasMorePages()) {
    if (!loadBooks(listQuery, listTitle, listPage + 1, booksFromFacet, PageLoad::Append, /*allowNetwork=*/false)) {
      if (!allowNetwork) return false;
      showLoadingBeforeFetch();
      loadBooks(listQuery, listTitle, listPage + 1, booksFromFacet, PageLoad::Append);
    }
    return state == BrowserState::BROWSING && loadedEnd() > previousEnd;
  }

  return false;
}

// The pages in front of the window were shown earlier in this session, so the list
// cache has them; the network only stands in for a page the cache failed to keep.
bool BookOrbitCatalogBrowserActivity::prependPreviousPage() {
  if (entries.empty() || entries.front().page <= 1) return false;
  const int page = entries.front().page - 1;
  const int previousStart = windowStart;
  if (navLevel == NavLevel::FacetList) {
    if (!loadFacetEntries(facetSectionId, facetTitle, page, PageLoad::Prepend, /*allowNetwork=*/false)) {
      showLoadingBeforeFetch();
      loadFacetEntries(facetSectionId, facetTitle, page, PageLoad::Prepend);
    }
  } else if (navLevel == NavLevel::Books) {
    if (!loadBooks(listQuery, listTitle, page, booksFromFacet, PageLoad::Prepend, /*allowNetwork=*/false)) {
      showLoadingBeforeFetch();
      loadBooks(listQuery, listTitle, page, booksFromFacet, PageLoad::Prepend);
    }
  } else {
    return false;
  }
  return state == BrowserState::BROWSING && windowStart < previousStart;
}

bool BookOrbitCatalogBrowserActivity::revealRow(const int row) {
  while (row < windowStart) {
    if (!prependPreviousPage()) return false;
  }
  return true;
}

bool BookOrbitCatalogBrowserActivity::rewindToStart() {
  if (navLevel == NavLevel::FacetList) {
    if (loadFacetEntries(facetSectionId, facetTitle, 1, PageLoad::Open, /*allowNetwork=*/false)) return true;
    showLoadingBeforeFetch();
    return loadFacetEntries(facetSectionId, facetTitle, 1);
  }
  if (navLevel == NavLevel::Books) {
    if (loadBooks(listQuery, listTitle, 1, booksFromFacet, PageLoad::Open, /*allowNetwork=*/false)) return true;
    showLoadingBeforeFetch();
    return loadBooks(listQuery, listTitle, 1, booksFromFacet);
  }
  return false;
}

bool BookOrbitCatalogBrowserActivity::wrapToEndFromCache() {
  while (hasMorePages()) {
    // The walk keeps nothing behind it: the selection lands on the listing's end.
    topIndex = selectorIndex = loadedEnd() - 1;
    if (!appendNextPageForCurrentList(/*allowNetwork=*/false)) break;
  }
  if (hasMorePages()) {
    if (windowStart > 0) {
      rewindToStart();
    } else {
      selectorIndex = topIndex = 0;
    }
    return false;
  }
  topIndex = std::max(windowStart, loadedEnd() - visibleRows);
  return true;
}

bool BookOrbitCatalogBrowserActivity::reopenListing(const NavLevel level, const int lastPage, const int wantedSelector,
                                                    const int wantedTop) {
  const auto loadPage = [this, level](const int page, const PageLoad load, const bool allowNetwork) {
    return level == NavLevel::FacetList ? loadFacetEntries(facetSectionId, facetTitle, page, load, allowNetwork)
                                        : loadBooks(listQuery, listTitle, page, booksFromFacet, load, allowNetwork);
  };
  for (int page = 1; page <= lastPage; page++) {
    const PageLoad load = page == 1 ? PageLoad::Open : PageLoad::Append;
    if (!loadPage(page, load, /*allowNetwork=*/false)) {
      showLoadingBeforeFetch();
      if (!loadPage(page, load, /*allowNetwork=*/true)) {
        return false;  // the load set the error state; keep whatever pages did load
      }
    }
    // Open put the listing at its top. The wanted viewport is what decides which
    // pages the rest of the walk drops.
    if (page == 1) topIndex = wantedTop;
  }
  if (entries.empty()) return false;
  topIndex = std::clamp(wantedTop, windowStart, loadedEnd() - 1);
  selectorIndex = std::clamp(wantedSelector, windowStart, loadedEnd() - 1);
  return true;
}

// Rebuild the book listing that downloadBook() freed, and put the selector back on
// the entry it was on
void BookOrbitCatalogBrowserActivity::restoreBookListAfterDownload() {
  reopenListing(NavLevel::Books, listPage, selectorIndex, topIndex);
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

void BookOrbitCatalogBrowserActivity::openBookListing(const BookOrbitBookQuery& query, const std::string& title,
                                                      const bool fromFacet) {
  {
    // Only the root and facet listings open a book listing, and their header does not
    // read this; the lock covers a render that would.
    RenderLock lock(*this);
    listDefaultReadStatus = query.readStatus;
  }
  if (!loadBooks(query, title, 1, fromFacet, PageLoad::Open, /*allowNetwork=*/false)) {
    showLoadingBeforeFetch();
    loadBooks(query, title, 1, fromFacet);
  }
}

bool BookOrbitCatalogBrowserActivity::listOptionsAvailable() const {
  // A shelf comes in the server's own order, which nothing on the request changes.
  return state == BrowserState::BROWSING && navLevel == NavLevel::Books && !listIsLocal && listQuery.shelf.empty();
}

void BookOrbitCatalogBrowserActivity::showListOptions(const ListOption option) {
  auto popup = makeUniqueNoThrow<OptionPopup>();
  if (!popup) {
    LOG_ERR("BookOrbit", "Cannot allocate the listing's %s picker", option == ListOption::Sort ? "sort" : "filter");
    return;
  }
  // Only records the pick: applying it fetches a listing, which must not run inside
  // the picker's input handling (it holds the render lock).
  auto onPick = [this](const int choice) { pendingListChoice = choice; };
  if (option == ListOption::Sort) {
    // The Library's button layout: the direction first, then the methods. The list view
    // rides along, so the sections can be turned off without leaving the catalog.
    const std::string listView = std::string(tr(STR_LIBRARY_LIST_VIEW)) + ": " +
                                 (SETTINGS.libraryListExpanded ? tr(STR_LIBRARY_EXPANDED) : tr(STR_COMPACT));
    const char* labels[SORT_ROW_FIRST_METHOD + SORT_CHOICE_COUNT];
    labels[SORT_ROW_DIRECTION] = sortsDescending(listQuery) ? "Z-A" : "A-Z";
    labels[SORT_ROW_LIST_VIEW] = listView.c_str();
    for (int i = 0; i < SORT_CHOICE_COUNT; i++) labels[SORT_ROW_FIRST_METHOD + i] = I18N.get(SORT_CHOICES[i].label);
    popup->show(tr(STR_LIBRARY_SORT_BY), labels, SORT_ROW_FIRST_METHOD + SORT_CHOICE_COUNT,
                SORT_ROW_FIRST_METHOD + sortChoiceIndex(listQuery.sort), std::move(onPick));
    popup->setDividerAfterOption(SORT_ROW_FIRST_METHOD - 1);
  } else {
    StrId labels[READ_STATUS_CHOICE_COUNT];
    for (int i = 0; i < READ_STATUS_CHOICE_COUNT; i++) labels[i] = READ_STATUS_CHOICES[i].label;
    popup->show(StrId::STR_BOOKORBIT_READ_STATUS, labels, READ_STATUS_CHOICE_COUNT,
                readStatusChoiceIndex(listQuery.readStatus), std::move(onPick));
  }
  popup->setDismissOnOutsideTouchDown(true);
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
  BookOrbitBookQuery query = listQuery;
  if (option == ListOption::Filter) {
    if (choice >= READ_STATUS_CHOICE_COUNT) return;
    query.readStatus = READ_STATUS_CHOICES[choice].id;
  } else if (choice == SORT_ROW_LIST_VIEW) {
    // The Library's own setting: its list shows the same sections, or not.
    SETTINGS.libraryListExpanded = SETTINGS.libraryListExpanded ? 0 : 1;
    if (!SETTINGS.saveToFile()) LOG_ERR("BookOrbit", "Cannot save the list view");
    requestUpdate();
    return;
  } else if (choice == SORT_ROW_DIRECTION) {
    // The direction row. Spelled out only where it departs from the sort's own, so
    // the query, and the cache key built from it, stay the same for the same order.
    const bool descending = !sortsDescending(query);
    query.order = descending == SORT_CHOICES[sortChoiceIndex(query.sort)].descendingFirst ? ""
                  : descending                                                            ? "desc"
                                                                                          : "asc";
  } else {
    const int method = choice - SORT_ROW_FIRST_METHOD;
    // The current method keeps its direction; another starts in its own, as in the Library.
    if (method >= SORT_CHOICE_COUNT || method == sortChoiceIndex(query.sort)) return;
    query.sort = SORT_CHOICES[method].id;
    query.order.clear();
  }
  if (query.sort == listQuery.sort && query.order == listQuery.order && query.readStatus == listQuery.readStatus) {
    return;
  }
  LOG_INF("BookOrbit", "Listing now sorted by %s%s%s, read status '%s'",
          query.sort.empty() ? SORT_CHOICES[DEFAULT_SORT_CHOICE].id : query.sort.c_str(),
          query.order.empty() ? "" : " ", query.order.c_str(), query.readStatus.c_str());
  reloadListing(query);
}

void BookOrbitCatalogBrowserActivity::reloadListing(const BookOrbitBookQuery& query) {
  // The listing on screen stays in memory until the new one has loaded, so a failed
  // request ends on the error screen, whose Back returns to it unchanged.
  if (!loadBooks(query, listTitle, 1, booksFromFacet, PageLoad::Open, /*allowNetwork=*/false)) {
    showLoadingBeforeFetch();
    if (!loadBooks(query, listTitle, 1, booksFromFacet)) return;
  }
  if (!entries.empty()) return;
  // A filter can leave nothing to show. Stay on the listing, whose pickers can undo it,
  // rather than on the error screen, whose Back would leave it.
  {
    RenderLock lock(*this);
    state = BrowserState::BROWSING;
  }
  requestUpdate();
}

std::string BookOrbitCatalogBrowserActivity::headerTitle() const {
  if (navLevel == NavLevel::FacetList && !facetTitle.empty()) return facetTitle;
  if (navLevel != NavLevel::Books || listTitle.empty()) return tr(STR_BOOKORBIT_CATALOG);
  std::string title = listTitle;
  if (!listIsLocal && !listQuery.readStatus.empty() && listQuery.readStatus != listDefaultReadStatus) {
    title += " \xC2\xB7 ";  // U+00B7 middle dot, in the built-in fonts' Latin-1 range
    title += I18N.get(READ_STATUS_CHOICES[readStatusChoiceIndex(listQuery.readStatus)].label);
  }
  // The listing's own total: a facet badge counts every format, and no badge knows the
  // filter. Only once it is on screen; a download boot draws this header before it loads.
  if (state == BrowserState::BROWSING) title += " (" + std::to_string(listTotal) + ")";
  return title;
}

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
                                                   const std::string& replacePath) {
  showDownloadScreen(title);

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

  // A re-download keeps the book where it is: its reading progress and caches are
  // keyed by that path. Either way the transfer is staged in a side file and only
  // swapped in once it is complete and opens as an EPUB (see downloadOptions below),
  // so a failure or a cancel never costs the user the copy they had, and a new book
  // never sits half-written under its own name.
  const std::string finalPath = replacePath.empty() ? catalogBookPath(detail, epubFile) : replacePath;
  // Create the download folder and, when the server's naming template nests the book
  // ("Sprawl/<book>.epub"), every folder it asks for. mkdir's pFlag creates the
  // missing parents in one call, so this covers both at once.
  const size_t lastSlash = finalPath.find_last_of('/');
  if (lastSlash > 0 && lastSlash != std::string::npos) {
    const std::string folder = finalPath.substr(0, lastSlash);
    if (!Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
      LOG_ERR("BookOrbit", "Could not create download folder %s", folder.c_str());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
  }

  if (restartForDownload(bookId, epubFile.id, title, finalPath)) return;
  transferBook(bookId, epubFile.id, finalPath);
}

bool BookOrbitCatalogBrowserActivity::restartForDownload(const int64_t bookId, const int64_t fileId,
                                                         const std::string& title, const std::string& path) {
#if FREEINK_MCU_C3
  BookOrbitDownloadResume resume;
  resume.bookId = bookId;
  resume.fileId = fileId;
  resume.title = title;
  resume.path = path;
  resume.listQuery = listQuery;
  resume.listDefaultReadStatus = listDefaultReadStatus;
  resume.listTitle = listTitle;
  resume.listPage = listPage;
  resume.booksFromFacet = booksFromFacet;
  resume.selectorIndex = selectorIndex;
  resume.topIndex = topIndex;
  resume.facetSectionId = facetSectionId;
  resume.facetTitle = facetTitle;
  resume.facetPage = facetPage;
  resume.facetSelectorIndex = facetSelectorIndex;
  resume.facetTopIndex = facetTopIndex;
  if (!BookOrbitCatalogListCache::saveDownloadResume(resume)) {
    LOG_ERR("BookOrbit", "Cannot save the download for its restart; downloading in place");
    return false;
  }
  LOG_INF("BookOrbit", "Restarting into the download of book %lld (row %d, top %d, page %d)",
          static_cast<long long>(bookId), selectorIndex, topIndex, listPage);
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
    // Backing out of Wi-Fi cancels the download: back to the listing, from the cache.
    restoreBookListAfterDownload();
    requestUpdate();
    return;
  }
  showDownloadScreen(job.title);
  transferBook(job.bookId, job.fileId, job.path);
}

void BookOrbitCatalogBrowserActivity::transferBook(const int64_t bookId, const int64_t fileId,
                                                   const std::string& finalPath) {
  // The heap the TLS session starts from: what tells a fragmented session from a
  // clean download boot in a field log. The version says which build wrote that log.
  LOG_INF("BookOrbit", "Downloading file %lld -> %s (free=%u maxAlloc=%u, firmware %s)", static_cast<long long>(fileId),
          finalPath.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), AppVersion::version());

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested) return true;
    mappedInput.update();
    if (mappedInput.wasHomeGesture()) {
      goHomeAfterCancel = true;
      cancelRequested = true;
    }
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
  downloadOptions.largeBody = true;
  // CrossInk v1.6.1's download safeguards, as its OPDS browser uses them: stage the file
  // as "<book>.part", refuse a book the card cannot hold before writing any of it, and
  // only put the file in place once it opens as an EPUB. A body cut short without a
  // Content-Length, or an error page served as the file, used to land as the book.
  downloadOptions.stageAsPart = true;
  downloadOptions.checkFreeSpace = true;
  downloadOptions.validate = [](const std::string& path) {
    ZipFile zip(path);
    size_t size = 0;
    return zip.getInflatedFileSize("META-INF/container.xml", &size) && size > 0;
  };

  // Free the current listing while the download runs: every KB of contiguous heap
  // matters next to the TLS session, and the list is rebuilt from listQuery after.
  {
    RenderLock lock(*this);
    std::vector<Entry>().swap(entries);
    listFreedForDownload = true;
  }

  constexpr int MAX_DOWNLOAD_ATTEMPTS = 3;
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  unsigned long now = 0;
  int percent = 0;
  for (int attempt = 0; attempt < MAX_DOWNLOAD_ATTEMPTS; attempt++) {
    // Honor a Back press between attempts too: a failing download otherwise runs
    // all its retries (TLS handshake included) with the cancel request ignored.
    if (pollCancel()) {
      result = HttpDownloader::ABORTED;
      break;
    }
    downloadOptions.resumePartial = attempt > 0;
    result = BookOrbitCatalogClient::downloadFile(
        fileId, finalPath,
        [this, &lastRenderedPercent, &lastProgressUpdateMs, &percent, &now](const size_t downloaded,
                                                                            const size_t total) {
          downloadProgress = downloaded;
          downloadTotal = total;
          percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
          now = millis();
          // Throttle e-ink refreshes to meaningful progress steps: partial
          // refreshes still wear the panel, and a raw byte-count callback fires
          // far more often than the screen needs to redraw.
          if (percent >= 100 || lastRenderedPercent < 0 ||
              (percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT &&
               now > DOWNLOAD_PROGRESS_MIN_UPDATE_MS) ||
              now - lastProgressUpdateMs > DOWNLOAD_PROGRESS_MAX_UPDATE_MS) {
            lastRenderedPercent = percent;
            lastProgressUpdateMs = now;
            requestUpdate(true);
          }
        },
        &cancelRequested, downloadOptions);
    // A full card will not have more room on the next attempt.
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED ||
        result == HttpDownloader::INSUFFICIENT_SPACE) {
      break;
    }
    LOG_ERR("BookOrbit", "Download attempt %d/%d failed (err=%d), retrying", attempt + 1, MAX_DOWNLOAD_ATTEMPTS,
            static_cast<int>(result));
  }

  const bool downloadFailed = result != HttpDownloader::OK && result != HttpDownloader::ABORTED;
  if (downloadFailed) {
    // preservePartial kept the staged file for resuming between attempts; don't
    // leave it behind once we give up.
    Storage.remove((finalPath + STAGED_DOWNLOAD_SUFFIX).c_str());
  }

  if (result == HttpDownloader::OK) {
    clearBookCache(finalPath);
    BookOrbitDownloadIndex::record(bookId, finalPath);
    // A new book, or new contents under a known path: the Library's index is stale.
    library::invalidateLibraryIndex();
  } else if (result == HttpDownloader::ABORTED) {
    LOG_DBG("BookOrbit", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    mappedInput.suppressNextBackRelease();
  } else {
    LOG_ERR("BookOrbit", "Download failed (err=%d)", static_cast<int>(result));
    errorMessage = result == HttpDownloader::INSUFFICIENT_SPACE ? tr(STR_SD_CARD_FULL) : tr(STR_DOWNLOAD_FAILED);
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

void BookOrbitCatalogBrowserActivity::activateSelected() {
  const Entry* selected = entryAt(selectorIndex);
  if (!selected) return;
  // A reference into the rows: the loads below copy what they need before replacing them.
  const auto& entry = *selected;
  switch (entry.type) {
    case EntryType::SECTION:
      openBookListing(BookOrbitCatalogClient::sectionBookQuery(entry.sectionId), titleWithoutBadge(entry),
                      /*fromFacet=*/false);
      break;
    case EntryType::FACET_SECTION:
      if (!loadFacetEntries(entry.sectionId, entry.title, 1, PageLoad::Open, /*allowNetwork=*/false)) {
        showLoadingBeforeFetch();
        loadFacetEntries(entry.sectionId, entry.title, 1);
      }
      break;
    case EntryType::LOCAL_SECTION:
      loadLocalBooks(entry.sectionId);
      break;
    case EntryType::LOCAL_BOOK:
      openLocalBook(entry.path);
      break;
    case EntryType::FACET: {
      // Mirror BookOrbit's own plugin (paramsForEntry): author filters by the
      // entry id; series prefers the numeric seriesId and sorts by series order;
      // collections, libraries and SmartScopes filter by their numeric id and
      // sort by title (the server's own booksHref for all three).
      BookOrbitBookQuery query;
      if (facetSectionId == "series") {
        query.sort = "series";
        if (!entry.seriesId.empty()) {
          query.seriesId = entry.seriesId;
        } else {
          query.series = entry.sectionId;
        }
      } else if (facetSectionId == "collections") {
        query.sort = "title";
        query.collectionId = entry.sectionId;
      } else if (facetSectionId == "libraries") {
        query.sort = "title";
        query.libraryId = entry.sectionId;
      } else if (facetSectionId == "smart-scopes") {
        query.sort = "title";
        query.smartScopeId = entry.sectionId;
      } else {
        query.author = entry.sectionId;
      }
      // Back reopens the facet listing on this row.
      facetSelectorIndex = selectorIndex;
      facetTopIndex = topIndex;
      openBookListing(query, titleWithoutBadge(entry), /*fromFacet=*/true);
      break;
    }
    case EntryType::SEARCH:
      launchSearch();
      break;
    case EntryType::BOOK:
      if (entry.onDevice) {
        openLocalBook(entry.path);
      } else {
        downloadBook(entry.bookId, entry.title);
      }
      break;
  }
}

void BookOrbitCatalogBrowserActivity::openLocalBook(const std::string& path) {
  // Pushing the reader here would never survive: onExit() reboots while the
  // radio is still up, so the reader is torn down before onEnter() records
  // the book. Persist the path and let the reboot land on it instead.
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  silentRestartToReader();
}

void BookOrbitCatalogBrowserActivity::showBookActionMenu(const bool ignoreInitialConfirmRelease) {
  const Entry* selected = entryAt(selectorIndex);
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
                           const Entry* current = entryAt(row);
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
  const Entry* book = entryAt(row);
  if (!book) return;
  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book->title),
                         [this, row](const ActivityResult& result) {
                           Entry* current = entryAt(row);
                           if (result.isCancelled || !current) return;
                           Entry& entry = *current;
                           BookActions::clearFileMetadata(entry.path);
                           if (!Storage.remove(entry.path.c_str())) {
                             LOG_ERR("BookOrbit", "Failed to delete book: %s", entry.path.c_str());
                             return;
                           }
                           RECENT_BOOKS.removeByPath(entry.path);
                           library::invalidateLibraryIndex();
                           // Re-detect rather than just clearing the flag: another copy (the SD root,
                           // /Read) may still match. The index drops its now-stale entry on lookup.
                           entry.onDevice = bookOnDevice(entry.bookId, entry.title, entry.subtitle, entry.path);
                           requestUpdate();
                         });
}

void BookOrbitCatalogBrowserActivity::navigateBack() {
  if (navLevel == NavLevel::Root) {
    onGoHome();
  } else if (navLevel == NavLevel::Books && booksFromFacet) {
    reopenListing(NavLevel::FacetList, facetPage, facetSelectorIndex, facetTopIndex);
  } else {
    if (!loadRoot(/*allowNetwork=*/false)) {
      showLoadingBeforeFetch();
      loadRoot();
    }
  }
}

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

  if (state == BrowserState::ERROR) {
    // Catalog browsing is a secondary feature: errors here just return you to the
    // previous list (or home from the root) rather than offering a retry, matching
    // the "no code beyond what's needed" scope decision for this feature.
    if (backRequested) {
      if (!BOOKORBIT_STORE.hasCredentials() || navLevel == NavLevel::Root) {
        onGoHome();
      } else if (entries.empty() && listFreedForDownload) {
        // The listing was freed for a download that then failed; rebuild it.
        restoreBookListAfterDownload();
      } else if (entries.empty()) {
        // The listing genuinely loaded empty (a section with no entries, a search
        // with no matches): reloading it would show the same error forever, so
        // Back navigates up, exactly as it does from a non-empty listing.
        navigateBack();
      } else {
        state = BrowserState::BROWSING;
        requestUpdate();
      }
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (backRequested) {
      onGoHome();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (longPressFired) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
      return;
    }
    // Holding Confirm on a catalog book opens its action menu instead of acting.
    const Entry* selected = entryAt(selectorIndex);
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
    using NavButtons = std::array<MappedInputManager::Button, 2>;
    const NavButtons nextButtons = listButtons
                                       ? NavButtons{MappedInputManager::Button::Down, MappedInputManager::Button::Down}
                                       : ButtonNavigator::getNextButtons();
    const NavButtons previousButtons = listButtons
                                           ? NavButtons{MappedInputManager::Button::Up, MappedInputManager::Button::Up}
                                           : ButtonNavigator::getPreviousButtons();

    if (!entries.empty()) {
      // Section headings can move the screen further down than this loop put it; start
      // from where the last frame really was.
      if (sectionsShown()) adoptRenderedViewport();
      // Rows the list actually drew, so page jumps land where the display pages;
      // a fixed constant would drift from the theme's row height. Every index below
      // counts rows from the start of the listing (see `entries`), so the pages a
      // move loads or drops leave the screen-page boundaries where they were.
      const int pageItems = visibleRows;
      // A move that could not reach its row (no network for a lost page) stays on
      // the rows in memory.
      const auto holdPosition = [this] {
        clampSelectionToWindow();
        requestUpdate();
      };
      // A move past the rows in memory: earlier pages come back from the list cache,
      // and a wrap to the top reopens the listing at its first page.
      const auto reachSelection = [this] {
        if (selectorIndex >= windowStart) return true;
        return selectorIndex == 0 ? rewindToStart() : revealRow(selectorIndex);
      };
      // Button navigation moves the selection; the viewport follows it.
      const auto followSelection = [this] {
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, loadedEnd());
        if (!revealRow(topIndex)) topIndex = windowStart;
      };
      // Swipes do the opposite: they move the viewport and leave the selection
      // where it is. Reaching the end of what is loaded pulls the next page in,
      // so a finger can walk a long listing the way the buttons already do.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        if (swipe == MappedInputManager::SwipeDir::Up) {
          const int wantedCount = topIndex + 2 * visibleRows;
          while (loadedEnd() < wantedCount && hasMorePages()) {
            if (!appendNextPageForCurrentList()) break;
          }
        }
        // A screen with section headings holds fewer rows: the next one starts where it ended.
        const int rowsOnScreen = screenRows();
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? rowsOnScreen : -rowsOnScreen;
        const int previousTop = topIndex;
        topIndex = scrollListBy(topIndex, delta, visibleRows, loadedEnd());
        if (!revealRow(topIndex)) topIndex = windowStart;
        if (topIndex != previousTop) requestUpdate();
        return;
      }
      // Prefetch at the page turn: after a forward move onto the last loaded
      // screen-page, append until that page is fully backed by loaded entries
      // (server pages are not screen-page multiples, and can even be smaller
      // than one screen). The load pause lands on a transition the e-ink
      // refreshes anyway instead of mid-page, and the page comes up full. The
      // lambdas read loadedEnd() live because appends extend the listing.
      const auto extendIfOnLastPage = [this, pageItems] {
        const int lastPageStart = (loadedEnd() - 1) / pageItems * pageItems;
        if (selectorIndex < lastPageStart) return;
        const int wantedCount = (selectorIndex / pageItems + 1) * pageItems;
        while (loadedEnd() < wantedCount && hasMorePages()) {
          if (!appendNextPageForCurrentList()) break;
        }
      };
      buttonNavigator.onRelease(nextButtons, [this, extendIfOnLastPage, followSelection, reachSelection, holdPosition] {
        if (selectorIndex + 1 >= loadedEnd() && hasMorePages() && !appendNextPageForCurrentList()) {
          requestUpdate();  // could not load past the end (e.g. network); hold position, no wrap
          return;
        }
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, loadedEnd());
        if (!reachSelection()) return holdPosition();
        extendIfOnLastPage();
        followSelection();
        requestUpdate();
      });
      // Backward from the very top: when the session cache holds the rest of the
      // listing, walk it (no requests) and wrap to the real end. A cache miss keeps
      // the clamp -- a Previous press should not start a network crawl.
      buttonNavigator.onRelease(previousButtons, [this, followSelection, reachSelection, holdPosition] {
        const int from = selectorIndex;
        if (from == 0 && hasMorePages() && !wrapToEndFromCache()) return holdPosition();
        selectorIndex = ButtonNavigator::previousIndex(from, loadedEnd());
        if (!reachSelection()) return holdPosition();
        followSelection();
        requestUpdate();
      });
      buttonNavigator.onContinuous(
          nextButtons, [this, pageItems, extendIfOnLastPage, followSelection, reachSelection, holdPosition] {
            if (selectorIndex / pageItems == (loadedEnd() - 1) / pageItems && hasMorePages() &&
                !appendNextPageForCurrentList()) {
              requestUpdate();
              return;
            }
            selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, loadedEnd(), pageItems);
            if (!reachSelection()) return holdPosition();
            extendIfOnLastPage();
            followSelection();
            requestUpdate();
          });
      buttonNavigator.onContinuous(previousButtons, [this, pageItems, followSelection, reachSelection, holdPosition] {
        const int from = selectorIndex;
        if (from / pageItems == 0 && hasMorePages()) {
          if (from > 0) {
            selectorIndex = 0;  // finish the backward run at the top first
            requestUpdate();
            return;
          }
          if (!wrapToEndFromCache()) return holdPosition();
        }
        selectorIndex = ButtonNavigator::previousPageIndex(from, loadedEnd(), pageItems);
        if (!reachSelection()) return holdPosition();
        followSelection();
        requestUpdate();
      });
    }
  }
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
    addListHeaderControls(screen, renderer, sortsDescending(listQuery));
  }
  // A filter that matches nothing leaves the header controls to undo it; render() says
  // the listing is empty.
  if (entries.empty()) return;

  const bool sections = sectionsShown();
  // Transient per-render: points into `entries`, freed on scope exit. The list gets
  // the rows in memory, so its indices are those rows' own (row - windowStart).
  std::vector<fui::ListItem> items;
  items.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); i++) {
    const auto& entry = entries[i];
    fui::ListItem item;
    item.label = entry.title.c_str();
    // The subtitle carries the author; without it two same-title search results
    // are indistinguishable.
    if (!entry.subtitle.empty()) item.subtitle = entry.subtitle.c_str();
    if (entry.onDevice) item.value = ON_DEVICE_MARKER;
    if (sections) item.sectionHeading = sectionHeading(i);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  const int selected = entryAt(selectorIndex) ? selectorIndex - windowStart : -1;
  props.selectedIndex = static_cast<int16_t>(selected);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);  // physical buttons stay in loop()
  props.valueInset = 8;  // air between the on-device dot and the row edge
  if (sections) configureUiListSectionHeaders(props, screen.theme());
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  // Clamped for this frame only: the loop owns topIndex, and a listing being rebuilt
  // around a wanted position (reopenListing) must not see it pulled into a partial window.
  // With sections the fixed-height clamp would pull back a top the last frame moved down
  // to fit the listing's end: only the window bounds it.
  int top = renderTop;
  if (renderPass == 0) {
    top = sections ? std::clamp(topIndex, windowStart, loadedEnd() - 1)
                   : std::max(windowStart, scrollListBy(topIndex, 0, visibleRows, loadedEnd()));
  }
  props.topIndex = static_cast<uint16_t>(top - windowStart);
  // A nav only to measure: it makes the list skip its fixed-height clamp too.
  if (sections) {
    listMeasure.reset();
    props.nav = &listMeasure;
  }

  const fui::Rect listRect = screen.body();
  screen.list(props);

  renderTop = top;
  if (sections) {
    const int drawn = listMeasure.drawnRows;
    const int relativeTop = top - windowStart;
    const int count = static_cast<int>(entries.size());
    int wanted = top;
    if (drawn > 0 && selected >= relativeTop && selected >= relativeTop + drawn) {
      // The selection fell below what fitted: down just far enough for it.
      wanted = top + selected - (relativeTop + drawn) + 1;
    } else if (drawn > 0 && !hasMorePages() && relativeTop + drawn < count && top >= loadedEnd() - visibleRows) {
      // The listing ends on this screen, but its last rows did not fit under the headings.
      wanted = top + count - (relativeTop + drawn);
      if (selected >= relativeTop) wanted = std::min(wanted, windowStart + selected);
    }
    if (wanted > top && renderPass + 1 < MAX_RENDER_PASSES) {
      renderTop = wanted;
      renderPassAgain = true;
      return;
    }
  }
  // What the loop may take for its next move (adoptRenderedViewport, screenRows).
  renderedTop.store(top);
  renderedRows.store(sections ? listMeasure.drawnRows : 0);
  renderedFromTop.store(renderFromTop);
  renderedFromSelection.store(renderFromSelection);
  renderedListing.store(listingGeneration, std::memory_order_release);

  // A long listing keeps only a few pages in memory, so the list sized its
  // indicator on that window: a 500-book listing would look like it ends one swipe
  // away, and a window deep in it like its top. Repaint the same track against the
  // whole listing instead -- the server's total for books, what has been reached
  // for facets, which give no total. Only when the list already drew one: that is
  // what shrank the rows to clear the track.
  const int scrollTotal = navLevel == NavLevel::Books ? std::max(listTotal, loadedEnd()) : loadedEnd();
  if ((windowStart > 0 || scrollTotal > loadedEnd()) && static_cast<int>(entries.size()) > visibleRows) {
    const auto& theme = screen.theme();
    fui::drawListScrollIndicator(screen.target(), listRect, static_cast<uint32_t>(scrollTotal),
                                 static_cast<uint32_t>(visibleRows), static_cast<uint32_t>(top), theme.listScrollWidth,
                                 theme.listScrollSide, theme.listScrollInset);
  }
}

void BookOrbitCatalogBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool listOptionsShown = listOptionsAvailable();

  const std::string title = headerTitle();
  const auto drawHeader = [this, &title, listOptionsShown] {
    if (mappedInput.hasTouchHardware() && listOptionsShown) {
      // The title stops short of the sort and filter controls.
      TouchHeaderBackButton::draw(
          renderer, TouchHeaderBackButton::compactHeaderRect(renderer), title.c_str(), /*readerContext=*/false,
          2 * HEADER_CONTROL_SIZE + HEADER_CONTROL_GAP + TouchHeaderBackButton::controlRightInset() + 10);
    } else if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::drawCompact(renderer, title.c_str());
    } else {
      CompactHeader::drawTitle(renderer, title.c_str());
    }
  };
  drawHeader();

  // WIFI_SELECTION: the Wi-Fi screen is only pushed on the next loop, and a network
  // boot paints the catalog before that, with no listing loaded yet.
  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING || state == BrowserState::WIFI_SELECTION) {
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
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, statusMessage.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (!entries.empty() || listOptionsShown) {
    uiReady = false;
    renderPass = 0;
    renderFromTop = topIndex;
    renderFromSelection = selectorIndex;
    // A pass that found the selection, or the listing's end, below the screen asks for
    // another one further down (see buildListScreen); each starts from a blank frame.
    for (;;) {
      renderPassAgain = false;
      app.render();
      if (!renderPassAgain) break;
      renderPass++;
      renderer.clearScreen();
      drawHeader();
    }
    uiReady = true;
  }
  if (entries.empty()) renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  // The picker draws its own button hints over the listing, then pushes the frame.
  if (listOptions && listOptions->processRender(renderer, mappedInput)) return;

  const Entry* selected = entryAt(selectorIndex);
  const bool onRemoteBook = selected && selected->type == EntryType::BOOK && !selected->onDevice;
  const char* confirmLabel = !selected ? "" : onRemoteBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const bool listButtons = listOptionsShown && !mappedInput.hasTouchHardware();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, listButtons ? tr(STR_SORT) : "",
                                            listButtons ? tr(STR_BOOKORBIT_FILTER) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
