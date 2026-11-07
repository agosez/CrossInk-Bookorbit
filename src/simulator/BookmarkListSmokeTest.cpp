#ifdef SIMULATOR
#include "BookmarkListSmokeTest.h"

#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "activities/ActivityManager.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/SavedItemsHomeActivity.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/EpubReaderBookmarkListActivity.h"
#include "activities/reader/EpubReaderMenuModel.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

struct BookmarkListSmokeTest {
  static constexpr size_t COUNT = 256;

  static void require(bool ok, const char* message) {
    if (ok) return;
    LOG_ERR("SMOKE", "Bookmarks: %s", message);
    std::_Exit(2);
  }

  template <typename T>
  static T& current() {
    auto* activity = dynamic_cast<T*>(activityManager.simulatorCurrentActivity());
    require(activity != nullptr, "unexpected activity");
    return *activity;
  }

  static void render() {
    require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "render failed");
  }

  static void capture(const char* name) {
    const char* output = std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES");
    if (!output) return;
    RenderLock lock;
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output) / (std::string(name) + ".pgm");
    FILE* image = std::fopen(path.c_str(), "wb");
    require(image != nullptr, "capture failed");
    std::fprintf(image, "P5\n%d %d\n255\n", renderer.getScreenWidth(), renderer.getScreenHeight());
    for (int y = 0; y < renderer.getScreenHeight(); ++y) {
      for (int x = 0; x < renderer.getScreenWidth(); ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
    }
    std::fclose(image);
  }

  static bool tick() {
    if (!std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOKMARKS")) return false;
    static unsigned phase = 0;
    switch (phase++) {
      case 0: {
        const char* path = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        require(path && BOOKMARKS.loadForBook(path, "Bookmark fixture", "Author", "epub"), "fixture load failed");
        BOOKMARKS.clearAll();
        for (size_t i = 0; i < COUNT; ++i) {
          require(
              BOOKMARKS.addBookmark(0, static_cast<float>(i) / COUNT, 0, "Chapter title", static_cast<uint16_t>(i + 1),
                                    "A saved passage from this chapter") == BookmarkStore::AddResult::Added,
              "fixture add failed");
        }
        BOOKMARKS.unload();
        SETTINGS.uiTheme = 4;  // The reported carousel Home stays behind its child screens.
        UITheme::getInstance().reload();
        activityManager.goHome();
        break;
      }
      case 1:
        render();
        current<HomeActivity>().onSavedItemsOpen();
        break;
      case 2: {
        render();
        auto& saved = current<SavedItemsHomeActivity>();
        require(saved.books.size() == 1 && saved.books[0].bookmarkCount == COUNT, "Home bookmark count");
        saved.openSavedItems(0);
        break;
      }
      case 3: {
        render();
        auto& list = current<EpubReaderBookmarkListActivity>();
        require(&list.bookmarks == &BOOKMARKS.getBookmarks(), "list copied the bookmark store");
        require(list.bookmarks.size() == COUNT, "list lost bookmarks");
        capture("bookmarks-home-first");
        {
          RenderLock lock;
          list.selectedIndex = COUNT - 1;
          list.topIndex = followListSelection(list.selectedIndex, 0, list.visibleRows, COUNT);
          freeink::ui::ListItem row;
          list.provideRow(&list, COUNT - 1, row);
          require(row.actionValue == COUNT - 1 && std::strcmp(row.value, "100%") == 0 &&
                      std::strcmp(row.subtitle, "Chapter title") == 0,
                  "last row lost its absolute index or label");
        }
        render();
        capture("bookmarks-home-last");
        {
          RenderLock lock;
          list.deleteSelectedBookmark();
          require(list.bookmarks.size() == COUNT - 1 && list.selectedIndex == COUNT - 2,
                  "deletion did not update the shared list/selection");
        }
        render();
        ActivityResult result;
        result.isCancelled = true;
        list.setResult(std::move(result));
        list.finish();
        break;
      }
      case 4: {
        render();
        auto& saved = current<SavedItemsHomeActivity>();
        require(saved.books[0].bookmarkCount == COUNT - 1, "Home count not refreshed after deletion");
        saved.openSavedItems(0);
        break;
      }
      case 5: {
        render();
        auto& list = current<EpubReaderBookmarkListActivity>();
        require(list.bookmarks.size() == COUNT - 1, "deletion not persisted on reopen");
        RenderLock lock;
        list.selectedIndex = 32;
        list.selectBookmark();
        break;
      }
      case 6:
        // The bookmark result first returns to SavedItems, then replaces the stack with the reader.
        if (!activityManager.isCurrentActivityNamed("EpubReader")) {
          require(activityManager.isCurrentActivityNamed("SavedItemsHome") ||
                      activityManager.isCurrentActivityNamed("Reader"),
                  "bookmark jump lost its route");
          --phase;
          return true;
        }
        render();
        current<EpubReaderActivity>().onReaderMenuConfirm(EpubReaderMenuAction::VIEW_BOOKMARKS);
        break;
      case 7: {
        render();
        auto& list = current<EpubReaderBookmarkListActivity>();
        require(list.bookmarks.size() == COUNT - 1 && &list.bookmarks == &BOOKMARKS.getBookmarks(),
                "reader bookmark list lost shared data");
        capture("bookmarks-reader-first");
        {
          RenderLock lock;
          BOOKMARKS.clearAll();
          list.selectedIndex = 0;
          list.topIndex = 0;
        }
        render();
        capture("bookmarks-empty");
        {
          RenderLock lock;
          list.selectBookmark();
          list.deleteSelectedBookmark();  // Empty-list actions remain harmless.
          require(list.bookmarks.empty(), "empty list gained bookmarks");
        }
        LOG_INF("SMOKE",
                "Simulator smoke test passed: bookmarks Home/reader, shared data, last row, deletion, reopen, jump and "
                "empty list");
        std::_Exit(0);
      }
    }
    return true;
  }
};

bool tickBookmarkListSmokeTest() { return BookmarkListSmokeTest::tick(); }
#endif
