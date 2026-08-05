#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Settings submenu for BookFusion: sign in/out, and entry point into the
 * library browser once signed in. Modeled on KOReaderSettingsActivity.
 */
class BookFusionSettingsActivity final : public Activity {
 public:
  explicit BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<12, 4>;

  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void handleSelection();
};
