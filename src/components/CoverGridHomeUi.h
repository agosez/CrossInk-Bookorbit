#pragma once

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "components/bars/tab-bar.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class CoverGridHomeUi final : public UiAppHost<16, 1> {
 public:
  using UiScreen = UiAppHost<16, 1>::Screen;
  static constexpr int THUMB_HEIGHT = 400;
  static constexpr int GRID_COLUMNS = 3;
  static constexpr int GRID_ROWS = 2;
  static constexpr int MAX_BOOKS = 1 + GRID_COLUMNS * GRID_ROWS;
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);
  // The tab bar's entries, in display order. Plugins and this fork's BookOrbit catalog only
  // take a slot once they are set up, so positions are resolved through the helpers
  // below rather than counted by hand.
  enum class Tab : uint8_t { Files, Library, Plugins, BookOrbit, Transfer, Settings, Count };
  static constexpr int MAX_TABS = static_cast<int>(Tab::Count);
  static int tabCount(bool hasPlugins, bool hasBookOrbit);
  // Tab::Count when the position is outside the bar.
  static Tab tabAt(int position, bool hasPlugins, bool hasBookOrbit);
  // -1 when the tab is not shown.
  static int tabPosition(Tab tab, bool hasPlugins, bool hasBookOrbit);
  explicit CoverGridHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasPlugins, bool hasBookOrbit, bool hasContinueReading,
             float featuredProgress);
  void refreshCoverPaths();
  void setSelection(int selection) { selected = selection; }
  int selectedAction(const MappedInputManager& input);
  // Exact generation size for a slot, recorded during draw. Rescaling a
  // dithered 1-bit image aliases badly.
  int thumbWidthFor(size_t index) const;
  int thumbHeightFor(size_t index) const;
  bool takeThumbHeightsChanged();

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawHeaderBand();
  void drawEmpty(UiScreen& screen);
  void drawCurrent(UiScreen& screen, freeink::ui::Rect rect, int coverRowHeight);
  void drawGrid(UiScreen& screen);
  freeink::ui::Rect layoutGrid(freeink::ui::Rect rect);
  void drawTabs(UiScreen& screen, freeink::ui::Rect rect);
  bool paintFramedCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, size_t index);
  void refreshCoverPath(size_t index);
  void noteThumbSize(size_t index, int slotWidth, int slotHeight);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> coverPaths;
  std::array<int, MAX_BOOKS> thumbWidths{};
  std::array<int, MAX_BOOKS> thumbHeights{};
  bool thumbHeightsChanged = false;
  int selected = 0;
  int pending = -1;
  int progress = -1;
  bool hasPlugins = false;
  bool hasBookOrbit = false;
  char progressText[12]{};
  // Component styles and interaction tables stay off the render task's stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
  freeink::ui::Rect gridBounds{};
  freeink::ui::TabBarProps tabs;
  std::array<freeink::ui::TabItem, MAX_TABS> tabItems;
};
