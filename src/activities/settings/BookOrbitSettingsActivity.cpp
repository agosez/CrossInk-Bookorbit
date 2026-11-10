#include "BookOrbitSettingsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitHomeStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/browser/BookOrbitCatalogBrowserActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/BookOrbitAuthActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/themes/bookorbit/BookOrbitHomeText.h"
#include "fontIds.h"
#include "network/SavedWifiConnect.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 9;
// The folder row reuses the OPDS strings: "Download Folder" and "SD Root" are
// generic and already translated in every language file.
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME,
                                     StrId::STR_PASSWORD,
                                     StrId::STR_BOOKORBIT_SERVER_URL,
                                     StrId::STR_AUTHENTICATE,
                                     StrId::STR_BOOKORBIT_CATALOG,
                                     StrId::STR_OPDS_DOWNLOAD_FOLDER,
                                     StrId::STR_SYNC_BEHAVIOR,
                                     StrId::STR_BOOKORBIT_SYNC_ON_SLEEP,
                                     StrId::STR_HOME};
constexpr int HOME_ROW = 8;
// The Home page: the top shelf, the bottom shelf, then the refresh.
constexpr int HOME_ITEMS = 3;
constexpr int REFRESH_ROW = 2;
const StrId homeNames[HOME_ITEMS] = {StrId::STR_HOME_TOP_SHELF, StrId::STR_HOME_BOTTOM_SHELF,
                                     StrId::STR_HOME_REFRESH_LISTS};
constexpr unsigned long FAILURE_MESSAGE_MS = 2500;
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

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const Page page, const bool networkBoot)
    : Activity("BookOrbitSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()),
      page(page),
      networkBoot(networkBoot) {}

int BookOrbitSettingsActivity::rowCount() const { return page == Page::Home ? HOME_ITEMS : MENU_ITEMS; }

void BookOrbitSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitSettingsActivity*>(user);
  if ((self->optionPopup && self->optionPopup->isActive()) || event.value < 0 || event.value >= self->rowCount()) {
    return;
  }
  self->selectedIndex = static_cast<size_t>(event.value);
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookOrbitSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = networkBoot ? REFRESH_ROW : 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitSettingsActivity::onRowEvent, this);
  app.setScreen(&BookOrbitSettingsActivity::listScreen, this);
  if (page == Page::Home) {
    optionPopup = makeUniqueNoThrow<OptionPopup>();
    if (!optionPopup) LOG_ERR("BookOrbit", "Cannot allocate the shelf pickers");
    // The network boot reads the list once its refresh is over: its TLS session comes first.
    if (!networkBoot) hasShelfOptions = BookOrbitHomeStore::loadShelfOptions(shelfOptions);
  }
  if (networkBoot) {
    fetching = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             // Out of this callback: the session belongs on loop()'s stack.
                             // Without Wi-Fi, loop() only reads the list already saved.
                             fetchOnline = !result.isCancelled;
                             fetchPending = true;
                           });
  }
  requestUpdate();
}

void BookOrbitSettingsActivity::onExit() { Activity::onExit(); }

void BookOrbitSettingsActivity::loop() {
  if (fetchPending) {
    fetchPending = false;
    fetchShelfOptions();
    return;
  }
  if (fetching) return;
  if (fetchFailed) {
    if (millis() - failedAt < FAILURE_MESSAGE_MS) return;
    fetchFailed = false;
    requestUpdate();
  }
  if (optionPopup && optionPopup->handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (page == Page::Home) {
      leaveHome();
    } else {
      finishAfterBackPress();
    }
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

  const int count = rowCount();
  buttonNavigator.onNext([this, count] {
    selectedIndex = (selectedIndex + 1) % count;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, count);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, count] {
    selectedIndex = (selectedIndex + count - 1) % count;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, count);
    requestUpdate();
  });
}

void BookOrbitSettingsActivity::handleSelection() {
  if (page == Page::Home) {
    handleHomeSelection();
  } else if (selectedIndex == 0) {
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
  } else if (selectedIndex == 7) {
    BOOKORBIT_STORE.setSyncOnSleep(!BOOKORBIT_STORE.getSyncOnSleep());
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == HOME_ROW) {
    startActivityForResult(std::make_unique<BookOrbitSettingsActivity>(renderer, mappedInput, Page::Home),
                           [](const ActivityResult&) {});
  }
}

void BookOrbitSettingsActivity::handleHomeSelection() {
  if (selectedIndex != REFRESH_ROW) {
    showShelfSources(static_cast<int>(selectedIndex));
    requestUpdate();
    return;
  }
  // Without an account there is no server to ask.
  if (BOOKORBIT_STORE.hasCredentials()) silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_HOME_SHELVES);
}

void BookOrbitSettingsActivity::showShelfSources(const int shelf) {
  if (!optionPopup) return;
  // The three sources every server has, then the collections and SmartScopes the last
  // refresh saved, collections first. Pointers into shelfOptions and the translations,
  // which the popup copies: 268 bytes of stack for the longest list, on the main loop.
  constexpr int BUILT_IN = static_cast<int>(BookOrbitHomeShelfKind::COLLECTION);
  const char* names[BUILT_IN + BookOrbitHomeStore::MAX_SHELF_OPTIONS];
  const BookOrbitHomeShelfSource& current = BOOKORBIT_STORE.getHomeShelf(shelf);
  int count = 0;
  int currentRow = 0;
  for (; count < BUILT_IN; count++) {
    const auto kind = static_cast<BookOrbitHomeShelfKind>(count);
    if (current.kind == kind) currentRow = count;
    names[count] = I18N.get(BookOrbitHomeText::shelfKindName(kind));
  }
  for (const BookOrbitHomeShelfSource& option : shelfOptions) {
    if (count >= BUILT_IN + BookOrbitHomeStore::MAX_SHELF_OPTIONS) break;
    if (option.sameAs(current)) currentRow = count;
    names[count++] = option.name;
  }
  optionPopup->show(I18N.get(shelf == 0 ? StrId::STR_HOME_TOP_SHELF : StrId::STR_HOME_BOTTOM_SHELF), names, count,
                    currentRow, [this, shelf](const int row) {
                      BookOrbitHomeShelfSource source;
                      if (row < BUILT_IN) {
                        source.kind = static_cast<BookOrbitHomeShelfKind>(row);
                      } else {
                        source = shelfOptions[static_cast<size_t>(row - BUILT_IN)];
                      }
                      chooseShelf(shelf, source);
                    });
  if (count > BUILT_IN) optionPopup->setDividerAfterOption(BUILT_IN - 1);
}

void BookOrbitSettingsActivity::chooseShelf(const int shelf, const BookOrbitHomeShelfSource& source) {
  LOG_INF("BookOrbit", "Home shelf %d now lists kind %d %s", shelf, static_cast<int>(source.kind), source.id);
  BOOKORBIT_STORE.setHomeShelf(shelf, source);
  BOOKORBIT_STORE.saveToFile();
}

void BookOrbitSettingsActivity::fetchShelfOptions() {
  bool fetched = false;
  if (fetchOnline) {
    sdFontSystem.releaseForNetwork(renderer);
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("BookOrbit", "Shelf options refresh could not be shown");
    }
    fetched = BookOrbitHomeStore::refreshShelfOptions();
    SavedWifiConnect::disconnect();
  }
  {
    RenderLock lock(*this);
    hasShelfOptions = BookOrbitHomeStore::loadShelfOptions(shelfOptions);
    fetching = false;
    fetchFailed = fetchOnline && !fetched;
  }
  failedAt = millis();
  requestUpdate();
}

void BookOrbitSettingsActivity::leaveHome() {
  if (UITheme::hasBookOrbitHome() && BOOKORBIT_STORE.hasCredentials() && !BookOrbitHomeStore::shelvesFollowSettings()) {
    LOG_INF("BookOrbit", "Home shelves changed: syncing the Home");
    silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_HOME_REFRESH);
    return;
  }
  // Wi-Fi has been up in this boot: only a restart gives its heap back whole.
  if (networkBoot) {
    silentRestart();
    return;
  }
  finishAfterBackPress();
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
  const int count = rowCount();
  std::vector<std::string> values(count);
  const bool hasCredentials = BOOKORBIT_STORE.hasCredentials();
  for (int i = 0; i < count; i++) {
    if (page == Page::Home) {
      if (i != REFRESH_ROW) {
        // The list gives a value all the width it asks for: a long collection name would
        // cover the row's label. Half the screen, measured in a font at least as wide as
        // the list's small one.
        values[i] =
            renderer.truncatedText(UI_10_FONT_ID, BookOrbitHomeText::shelfTitle(BOOKORBIT_STORE.getHomeShelf(i)),
                                   renderer.getScreenWidth() / 2);
      } else if (hasShelfOptions) {
        unsigned collections = 0;
        for (const BookOrbitHomeShelfSource& option : shelfOptions) {
          if (option.kind == BookOrbitHomeShelfKind::COLLECTION) collections++;
        }
        char counts[48];
        snprintf(counts, sizeof(counts), tr(STR_HOME_SHELF_OPTIONS_FORMAT), collections,
                 static_cast<unsigned>(shelfOptions.size()) - collections);
        values[i] = counts;
      }
    } else if (i == 0) {
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
    } else if (i == 7) {
      values[i] = BOOKORBIT_STORE.getSyncOnSleep() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == HOME_ROW) {
      // A page of its own: no value.
    } else {
      // Authenticate and Browse Catalog both need credentials to do anything.
      values[i] = hasCredentials ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }
  }

  std::vector<fui::ListItem> items;
  items.reserve(count);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    item.label = I18N.get(page == Page::Home ? homeNames[i] : menuNames[i]);
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
  topIndex = scrollListBy(topIndex, 0, visibleRows, count);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitSettingsActivity::render(RenderLock&&) {
  // The rows are drawn again under every popup: the name popup replaces the kind popup,
  // and a smaller dialog would leave the larger one's edges on screen.
  renderer.clearScreen();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const char* title = page == Page::Home ? tr(STR_HOME) : tr(STR_BOOKORBIT_SYNC);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  uiReady = false;
  app.render();
  uiReady = true;
  if (optionPopup && optionPopup->processRender(renderer, mappedInput)) return;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (fetching || fetchFailed) {
    GUI.drawPopup(renderer, fetching ? tr(STR_LOADING_POPUP) : tr(STR_BOOKORBIT_CATALOG_ERROR));
  }

  renderer.displayBuffer();
}
