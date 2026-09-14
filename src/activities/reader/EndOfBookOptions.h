#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

// Shared End-of-Book next-book menu for the EPUB and XTC readers. Collects up to
// MAX_SUGGESTIONS sibling books once per reader session, handles the menu input, and
// draws the end screen. With no suggestions the end screen keeps its historical
// plain-title look and behavior.
class EndOfBookOptions {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  explicit EndOfBookOptions(GfxRenderer& renderer);

  // Scans the book's folder for suggestions; no-op when already loaded. Call while
  // serialized by RenderLock. The loaded flag is the release/acquire publication point
  // that lets the other task read the finished list safely.
  void loadOnce(const std::string& currentBookPath);

  // True once the original folder scan has completed, including when it found no books.
  bool loaded() const;

  // True when the suggestion menu is showing and should own the reader's input.
  bool menuActive() const;

  // Menu input handling, following the standard list idiom: a tap on a row opens it
  // (or Home), side Up/Down and front Left/Right move the selection (wrapping),
  // Confirm opens the selection, and a short Back press returns to the last page of
  // the book. Fills openPath when the result is OpenBook. Returns Action::None when
  // nothing relevant was pressed; callers continue their normal input path (keeping
  // long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the full end screen (plain title, or the suggestion menu) onto a cleared buffer.
  void render(GfxRenderer& renderer, const MappedInputManager& input);

 private:
  // FreeInkApp hosts the suggestion list (themed rows, touch routing); the title and
  // button hints stay on the legacy UITheme calls. 4 rows (3 suggestions + Home) is
  // the whole interaction surface; 2 handler slots give the row action headroom.
  using UiApp = freeink::ui::FreeInkApp<6, 2>;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  GfxRenderer& renderer;
  std::string folder;
  // Written by the render task in loadOnce(), immutable afterwards; the main task only
  // reads it after isLoaded is observed true (acquire), so no further locking is needed.
  std::vector<std::string> names;
  // Built once with the suggestions so render() does not allocate on each e-ink refresh.
  std::vector<std::string> rowLabels;
  std::array<freeink::ui::ListItem, MAX_SUGGESTIONS + 1> rowItems{};
  uint16_t rowCount = 0;
  int selector = 0;
  std::atomic<bool> isLoaded{false};

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; handleMenuInput() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};

  std::string fullPath(size_t index) const;
};
