#include "BookFusionSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookFusionAuthActivity.h"
#include "BookFusionBrowserActivity.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 2;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_BF_ACCOUNT, StrId::STR_BF_BROWSE_LIBRARY};
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

BookFusionSettingsActivity::BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookFusionSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BookFusionSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookFusionSettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookFusionSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &BookFusionSettingsActivity::onRowEvent, this);
  app.setScreen(&BookFusionSettingsActivity::listScreen, this);
  requestUpdate();
}

void BookFusionSettingsActivity::onExit() { Activity::onExit(); }

void BookFusionSettingsActivity::loop() {
  auto activateSelected = [this] { handleSelection(); };

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
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

void BookFusionSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Account: sign in if signed out, confirm-then-sign-out if signed in.
    if (BOOKFUSION_STORE.hasToken()) {
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BF_SIGN_OUT),
                                                                    tr(STR_BF_SIGN_OUT_CONFIRM)),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 BOOKFUSION_STORE.clearTokens();
                               }
                               requestUpdate();
                             });
    } else {
      startActivityForResult(std::make_unique<BookFusionAuthActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  } else if (selectedIndex == 1) {
    // Browse Library: only reachable once signed in (row is disabled otherwise).
    if (!BOOKFUSION_STORE.hasToken()) return;
    startActivityForResult(std::make_unique<BookFusionBrowserActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  }
}

void BookFusionSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookFusionSettingsActivity*>(user)->buildListScreen(screen);
}

void BookFusionSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool signedIn = BOOKFUSION_STORE.hasToken();
  std::vector<std::string> values(MENU_ITEMS);
  values[0] = signedIn ? tr(STR_BF_SIGNED_IN) : tr(STR_BF_SIGNED_OUT);
  values[1] = signedIn ? "" : std::string("[") + tr(STR_BF_SIGN_IN_FIRST) + "]";

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
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookFusionSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BF_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BF_SYNC));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
