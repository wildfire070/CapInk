#include "BookFusionBrowserActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
constexpr size_t DOWNLOAD_BUFFER_SIZE = 2048;

struct Category {
  StrId nameId;
  const char* list;  // BookFusion "list" filter, nullptr for "all books".
};

// list values match BookFusion's own book-list filters.
constexpr Category CATEGORIES[] = {
    {StrId::STR_BF_CURRENTLY_READING, "currently_reading"},
    {StrId::STR_BF_FAVORITES, "favorites"},
    {StrId::STR_BF_PLAN_TO_READ, "planned_to_read"},
    {StrId::STR_BF_COMPLETED, "completed"},
    {StrId::STR_BF_ALL_BOOKS, nullptr},
};
constexpr int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

std::string buildBookFilename(const BookFusionBook& book) {
  if (book.author.empty()) return book.title;
  if (book.title.empty()) return book.author;
  return book.title + " - " + book.author;
}
}  // namespace

BookFusionBrowserActivity::BookFusionBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookFusionBrowser", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BookFusionBrowserActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  state = BrowserState::CHECK_WIFI;
  selectedCategory = 0;
  currentCategory = 0;
  page = BookFusionSearchResult{};
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  uiReady = false;
  visibleRows = 1;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &BookFusionBrowserActivity::onRowEvent, this);
  app.on(ACTION_CANCEL, &BookFusionBrowserActivity::onCancelEvent, this);
  app.setScreen(&BookFusionBrowserActivity::rootScreen, this);
  requestUpdate();

  if (!BookFusionSyncClient::getBearerToken().empty()) {
    checkAndConnectWifi();
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_BF_NO_TOKEN_MSG);
    requestUpdate();
  }
}

void BookFusionBrowserActivity::onExit() {
  Activity::onExit();
  page = BookFusionSearchResult{};

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

bool BookFusionBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
      return true;
    case BrowserState::CATEGORY_SELECTION:
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

// Row layout in BROWSING: [Previous Page?] [...books...] [Next Page?].
// Returns the book index for a row, or -1 if the row is a page-nav row.
namespace {
int bookIndexForRow(const BookFusionSearchResult& page, int row) {
  const int prevOffset = page.currentPage > 0 ? 1 : 0;
  if (row < prevOffset) return -1;  // "Previous Page"
  const int bookRow = row - prevOffset;
  if (bookRow < static_cast<int>(page.books.size())) return bookRow;
  return -1;  // "Next Page"
}
}  // namespace

void BookFusionBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  self->app.clearTapFlash();

  if (self->state == BrowserState::CATEGORY_SELECTION) {
    if (event.value < 0 || event.value >= NUM_CATEGORIES) return;
    self->selectCategory(event.value);
    return;
  }

  if (self->state != BrowserState::BROWSING) return;
  const int prevOffset = self->page.currentPage > 0 ? 1 : 0;
  const int rowCount = prevOffset + static_cast<int>(self->page.books.size()) + (self->page.hasMore ? 1 : 0);
  if (event.value < 0 || event.value >= rowCount) return;
  self->selectorIndex = event.value;

  const int bookIndex = bookIndexForRow(self->page, event.value);
  if (bookIndex >= 0) {
    self->downloadBook(self->page.books[bookIndex]);
  } else if (event.value < prevOffset) {
    self->loadPage(self->page.currentPage - 1);
  } else {
    self->loadPage(self->page.currentPage + 1);
  }
}

void BookFusionBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void BookFusionBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION) return;

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      checkAndConnectWifi();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::CATEGORY_SELECTION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      selectCategory(selectedCategory);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        if (app.invalidated()) requestUpdate();
        if (event) return;
      }
    }
    buttonNavigator.onNextRelease([this] {
      selectedCategory = ButtonNavigator::nextIndex(selectedCategory, NUM_CATEGORIES);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectedCategory = ButtonNavigator::previousIndex(selectedCategory, NUM_CATEGORIES);
      requestUpdate();
    });
    return;
  }

  if (state == BrowserState::BROWSING) {
    const int prevOffset = page.currentPage > 0 ? 1 : 0;
    const int rowCount = prevOffset + static_cast<int>(page.books.size()) + (page.hasMore ? 1 : 0);

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::CATEGORY_SELECTION;
      requestUpdate();
      return;
    }

    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        if (app.invalidated()) requestUpdate();
        if (event) return;
        if (state != BrowserState::BROWSING) return;
      }
    }

    if (rowCount > 0) {
      const auto moveSelection = [this, rowCount](const int index) {
        selectorIndex = index;
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, rowCount);
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, &moveSelection, rowCount] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, rowCount)); });
      buttonNavigator.onPreviousRelease(
          [this, &moveSelection, rowCount] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, rowCount)); });
    }
  }
}

void BookFusionBrowserActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::CATEGORY_SELECTION:
      self->buildCategoryScreen(screen);
      break;
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

void BookFusionBrowserActivity::screenHeader(UiApp::ScreenType& screen, const char* title) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  fui::HeaderProps header;
  header.title = title;
  header.borderEdges = fui::EdgeBottom;
  screen.header(header);
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void BookFusionBrowserActivity::buildCategoryScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BF_BROWSE_LIBRARY));

  std::vector<fui::ListItem> items;
  items.reserve(NUM_CATEGORIES);
  for (int i = 0; i < NUM_CATEGORIES; i++) {
    fui::ListItem item;
    item.label = I18N.get(CATEGORIES[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedCategory);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  screen.list(props);
}

void BookFusionBrowserActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, I18N.get(CATEGORIES[currentCategory].nameId));

  const int prevOffset = page.currentPage > 0 ? 1 : 0;
  const int rowCount = prevOffset + static_cast<int>(page.books.size()) + (page.hasMore ? 1 : 0);
  if (rowCount == 0) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  std::vector<fui::ListItem> items;
  items.reserve(rowCount);
  if (prevOffset > 0) {
    fui::ListItem prevItem;
    prevItem.label = tr(STR_PREV_PAGE);
    prevItem.actionValue = 0;
    items.push_back(prevItem);
  }
  for (size_t i = 0; i < page.books.size(); ++i) {
    const auto& book = page.books[i];
    fui::ListItem item;
    item.label = book.title.c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    item.actionValue = static_cast<int16_t>(prevOffset + i);
    items.push_back(item);
  }
  if (page.hasMore) {
    fui::ListItem nextItem;
    nextItem.label = tr(STR_NEXT_PAGE);
    nextItem.actionValue = static_cast<int16_t>(rowCount - 1);
    items.push_back(nextItem);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, rowCount);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookFusionBrowserActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_DOWNLOADING));

  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void BookFusionBrowserActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BF_BROWSE_LIBRARY));

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  screen.centeredText(statusMessage.c_str(), centered);
}

void BookFusionBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::CATEGORY_SELECTION:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case BrowserState::BROWSING:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  app.render();
  uiReady = true;
  renderer.displayBuffer();
}

void BookFusionBrowserActivity::showLoadingBeforeFetch() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFBrowser", "Loading screen could not be rendered before fetch");
    requestUpdate(true);
  }
}

void BookFusionBrowserActivity::selectCategory(int index) {
  if (index < 0 || index >= NUM_CATEGORIES) return;
  selectedCategory = index;
  currentCategory = index;
  selectorIndex = 0;
  topIndex = 0;
  showLoadingBeforeFetch();
  loadPage(0);
}

void BookFusionBrowserActivity::loadPage(int pageIndex) {
  if (pageIndex < 0) return;
  showLoadingBeforeFetch();

  BookFusionSearchResult result;
  const auto err = BookFusionSyncClient::searchBooks(pageIndex, CATEGORIES[currentCategory].list, result);
  if (err != BookFusionSyncClient::OK) {
    state = BrowserState::ERROR;
    errorMessage = BookFusionSyncClient::errorString(err);
    requestUpdate();
    return;
  }

  page = std::move(result);
  selectorIndex = 0;
  topIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
}

void BookFusionBrowserActivity::downloadBook(const BookFusionBook& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

  std::string downloadUrl;
  const auto urlErr = BookFusionSyncClient::getDownloadUrl(book.bookId, downloadUrl);
  if (urlErr != BookFusionSyncClient::OK) {
    state = BrowserState::ERROR;
    errorMessage = BookFusionSyncClient::errorString(urlErr);
    requestUpdate();
    return;
  }

  const char* downloadFolder = SETTINGS.opdsDownloadFolder;
  const bool useDownloadFolder = downloadFolder[0] != '\0';
  if (useDownloadFolder && !Storage.exists(downloadFolder) && !Storage.mkdir(downloadFolder)) {
    LOG_ERR("BFBrowser", "Could not create download folder %s", downloadFolder);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  std::string filename;
  filename.reserve(96);
  if (useDownloadFolder) filename += downloadFolder;
  filename += '/';
  filename += StringUtils::sanitizeFilename(buildBookFilename(book));
  filename += ".epub";
  LOG_DBG("BFBrowser", "Downloading book %lu -> %s", (unsigned long)book.bookId, filename.c_str());

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested || cancelDownload) {
      cancelRequested = true;
      return true;
    }
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
  downloadOptions.bufferSize = DOWNLOAD_BUFFER_SIZE;
  downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  downloadOptions.preservePartial = true;
  downloadOptions.resumePartial = true;
  // Defense-in-depth only: the URL from getDownloadUrl() is expected to be
  // pre-signed, and this header is only ever sent same-origin with it (see
  // HttpDownloader's redirect gating), so it's a no-op if the URL doesn't need it.
  downloadOptions.bearerToken = BookFusionSyncClient::getBearerToken();
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        mappedInput.update();
        if (uiReady) {
          const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
          if (snap.touchPressed || snap.touchReleased) app.route(snap);
        }
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested, "", "", downloadOptions);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    BookFusionBookIdStore::saveBookId(filename, book.bookId);
    state = BrowserState::BROWSING;
  } else if (result == HttpDownloader::ABORTED) {
    LOG_INF("BFBrowser", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    mappedInput.suppressNextBackRelease();
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void BookFusionBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::CATEGORY_SELECTION;
    requestUpdate();
    return;
  }
  launchWifiSelection();
}

void BookFusionBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::CATEGORY_SELECTION;
    requestUpdate();
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
