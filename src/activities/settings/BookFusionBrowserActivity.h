#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * BookFusion library browser: pick a category, then page through books in
 * it, downloading EPUBs with cancel/resume support. Modeled closely on
 * OpdsBookBrowserActivity's state machine and download handling, adapted for
 * BookFusion's flat category+page API instead of OPDS feed navigation.
 */
class BookFusionBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, CATEGORY_SELECTION, BROWSING, DOWNLOADING, ERROR };

  explicit BookFusionBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  using UiApp = freeink::ui::FreeInkApp<24, 4>;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;

  int selectedCategory = 0;  // Highlighted row on the category screen.
  int currentCategory = 0;   // Category currently being browsed.

  BookFusionSearchResult page;  // Current page of books (vector capacity reserved to BOOKFUSION_BOOKS_PER_PAGE).
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;

  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelDownload = false;
  bool goHomeAfterCancel = false;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen, const char* title);
  void buildCategoryScreen(UiApp::ScreenType& screen);
  void buildBrowsingScreen(UiApp::ScreenType& screen);
  void buildDownloadScreen(UiApp::ScreenType& screen);
  void buildStatusScreen(UiApp::ScreenType& screen);

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void loadPage(int pageIndex);
  void selectCategory(int index);
  void downloadBook(const BookFusionBook& book);
};
