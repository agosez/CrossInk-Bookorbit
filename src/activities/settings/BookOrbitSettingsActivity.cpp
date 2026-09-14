#include "BookOrbitSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookOrbitCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/browser/BookOrbitCatalogBrowserActivity.h"
#include "activities/settings/BookOrbitAuthActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 7;
// The folder row reuses the OPDS strings: "Download Folder" and "SD Root" are
// generic and already translated in every language file.
const StrId menuNames[MENU_ITEMS] = {
    StrId::STR_USERNAME,     StrId::STR_PASSWORD,          StrId::STR_BOOKORBIT_SERVER_URL,
    StrId::STR_AUTHENTICATE, StrId::STR_BOOKORBIT_CATALOG, StrId::STR_OPDS_DOWNLOAD_FOLDER,
    StrId::STR_SYNC_BEHAVIOR};
constexpr fui::ActionId ACTION_ROW = 1;

// Mirrors OpdsServerListActivity's normalizeDownloadFolder: "" for the SD root,
// otherwise a leading and no trailing slash, so folder + "/file.epub" is a path.
std::string normalizeDownloadFolder(std::string folder) {
  while (!folder.empty() && (folder.front() == ' ' || folder.front() == '\t')) folder.erase(folder.begin());
  while (!folder.empty() && (folder.back() == ' ' || folder.back() == '\t')) folder.pop_back();
  if (folder.empty() || folder == "/") return "";
  if (folder.front() != '/') folder.insert(folder.begin(), '/');
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  return folder;
}
}  // namespace

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookOrbitSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BookOrbitSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitSettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookOrbitSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitSettingsActivity::onRowEvent, this);
  app.setScreen(&BookOrbitSettingsActivity::listScreen, this);
  requestUpdate();
}

void BookOrbitSettingsActivity::onExit() { Activity::onExit(); }

void BookOrbitSettingsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });
}

void BookOrbitSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_USERNAME),
                                                                   BOOKORBIT_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               BOOKORBIT_STORE.setCredentials(kb.text, BOOKORBIT_STORE.getPassword());
                               BOOKORBIT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Password
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_PASSWORD),
                                                                   BOOKORBIT_STORE.getPassword(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               BOOKORBIT_STORE.setCredentials(BOOKORBIT_STORE.getUsername(), kb.text);
                               BOOKORBIT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    // Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = BOOKORBIT_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               BOOKORBIT_STORE.setServerUrl(urlToSave);
                               BOOKORBIT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 3) {
    // Authenticate
    if (!BOOKORBIT_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    // Replaces this screen rather than stacking on top of it, so the TLS handshake gets
    // the memory the settings stack was holding (see ActivityManager::goToBookOrbitAuth).
    activityManager.goToBookOrbitAuth();
  } else if (selectedIndex == 4) {
    // Browse Catalog
    if (!BOOKORBIT_STORE.hasCredentials()) {
      return;
    }
    activityManager.goToBookOrbitCatalog();
  } else if (selectedIndex == 5) {
    // Catalog download folder ("" = SD root); created on first download.
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER),
                                                BOOKORBIT_STORE.getDownloadFolder(), 64, InputType::Text),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT_STORE.setDownloadFolder(normalizeDownloadFolder(kb.text));
            BOOKORBIT_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 6) {
    const auto current = BOOKORBIT_STORE.getSyncBehavior();
    BOOKORBIT_STORE.setSyncBehavior(current == BookOrbitSyncBehavior::ASK_EVERY_TIME
                                        ? BookOrbitSyncBehavior::SMART
                                        : BookOrbitSyncBehavior::ASK_EVERY_TIME);
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  }
}

void BookOrbitSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookOrbitSettingsActivity*>(user)->buildListScreen(screen);
}

void BookOrbitSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(MENU_ITEMS);
  const bool hasCredentials = BOOKORBIT_STORE.hasCredentials();
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == 0) {
      const auto username = BOOKORBIT_STORE.getUsername();
      values[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == 1) {
      values[i] = BOOKORBIT_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == 2) {
      const auto serverUrl = BOOKORBIT_STORE.getServerUrl();
      values[i] = serverUrl.empty() ? tr(STR_NOT_SET) : serverUrl;
    } else if (i == 5) {
      const std::string& folder = BOOKORBIT_STORE.getDownloadFolder();
      values[i] = folder.empty() ? tr(STR_OPDS_SD_ROOT) : folder;
    } else if (i == 6) {
      values[i] = BOOKORBIT_STORE.getSyncBehavior() == BookOrbitSyncBehavior::SMART ? tr(STR_SMART_SYNC)
                                                                                    : tr(STR_ASK_EVERY_TIME);
    } else {
      // Authenticate and Browse Catalog both need credentials to do anything.
      values[i] = hasCredentials ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }
  }

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BOOKORBIT_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_SYNC));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
