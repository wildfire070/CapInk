#include "BookFusionAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "BookFusionSyncClient.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"
#include "util/QrUtils.h"

namespace {
// Only re-render the countdown every 10s to limit e-ink refreshes while
// waiting for the user to approve the code on another device.
constexpr unsigned long TIMER_REFRESH_INTERVAL_MS = 10000;
constexpr int QR_CODE_SIZE = 198;
}  // namespace

void BookFusionAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = REQUESTING_CODE;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFAuth", "Requesting-code screen could not be rendered before request");
    requestUpdate(true);
  }

  requestCode();
}

void BookFusionAuthActivity::requestCode() {
  BookFusionDeviceAuth auth;
  const auto result = BookFusionSyncClient::startDeviceAuth(auth);

  if (result != BookFusionSyncClient::OK) {
    LOG_ERR("BFAuth", "startDeviceAuth failed: %s", BookFusionSyncClient::errorString(result).c_str());
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = BookFusionSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  deviceCode = auth.deviceCode;
  userCode = auth.userCode;
  verificationUri = auth.verificationUri;
  verificationUriComplete = auth.verificationUriComplete.empty() ? auth.verificationUri : auth.verificationUriComplete;
  pollIntervalSec = auth.interval;

  const unsigned long now = millis();
  pollExpireAt = now + static_cast<unsigned long>(auth.expiresIn) * 1000UL;
  nextPollAt = now + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
  lastTimerRefresh = now;
  networkRetries = 0;

  {
    RenderLock lock(*this);
    state = WAITING_FOR_USER;
  }
  requestUpdate();
}

void BookFusionAuthActivity::doPoll() {
  {
    RenderLock lock(*this);
    state = POLLING;
  }

  const auto result = BookFusionSyncClient::pollForToken(deviceCode);
  bool needsUpdate = false;

  switch (result) {
    case BookFusionSyncClient::OK: {
      RenderLock lock(*this);
      state = SUCCESS;
      needsUpdate = true;
      break;
    }
    case BookFusionSyncClient::AUTH_PENDING: {
      nextPollAt = millis() + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
      RenderLock lock(*this);
      state = WAITING_FOR_USER;
      break;
    }
    case BookFusionSyncClient::SLOW_DOWN: {
      pollIntervalSec += 5;
      nextPollAt = millis() + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
      RenderLock lock(*this);
      state = WAITING_FOR_USER;
      break;
    }
    case BookFusionSyncClient::EXPIRED:
    case BookFusionSyncClient::AUTH_FAILED: {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = BookFusionSyncClient::errorString(result);
      needsUpdate = true;
      break;
    }
    case BookFusionSyncClient::NETWORK_ERROR: {
      networkRetries++;
      RenderLock lock(*this);
      if (networkRetries > MAX_NETWORK_RETRIES) {
        state = FAILED;
        errorMessage = BookFusionSyncClient::errorString(result);
        needsUpdate = true;
      } else {
        nextPollAt = millis() + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
        state = WAITING_FOR_USER;
      }
      break;
    }
    default: {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = BookFusionSyncClient::errorString(result);
      needsUpdate = true;
      break;
    }
  }

  if (needsUpdate) requestUpdate();
}

void BookFusionAuthActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionAuthActivity::onExit() { Activity::onExit(); }

void BookFusionAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }
    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == WAITING_FOR_USER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }

    const unsigned long now = millis();

    if (now - pollExpireAt < 0x80000000UL && now >= pollExpireAt) {
      {
        RenderLock lock(*this);
        state = FAILED;
        errorMessage = tr(STR_BF_AUTH_EXPIRED);
      }
      requestUpdate();
      return;
    }

    if (now - lastTimerRefresh >= TIMER_REFRESH_INTERVAL_MS) {
      lastTimerRefresh = now;
      requestUpdate();
    }

    if (now >= nextPollAt) {
      doPoll();
    }
  }
}

void BookFusionAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if ((state == SUCCESS || state == FAILED) && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BF_AUTH), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BF_AUTH));
  }

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop =
      metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;

  if (state == REQUESTING_CODE || state == CONNECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, tr(STR_BF_WAITING));
  } else if (state == WAITING_FOR_USER || state == POLLING) {
    int y = contentTop;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BF_VISIT_URL), true, EpdFontFamily::BOLD);
    y += lineH + 4;

    renderer.drawCenteredText(UI_10_FONT_ID, y, verificationUri.c_str(), true, EpdFontFamily::REGULAR);
    y += lineH + 4;

    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BF_ENTER_CODE), true, EpdFontFamily::REGULAR);
    y += lineH + 4;

    renderer.drawCenteredText(UI_12_FONT_ID, y, userCode.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 8;

    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BF_OR_SCAN_QR), true, EpdFontFamily::REGULAR);
    y += lineH + 8;

    // QR encodes verification_uri_complete (code pre-filled) when BookFusion
    // sends one; otherwise it falls back to the bare verification_uri and the
    // user still types the code shown above.
    const Rect qrBounds((pageWidth - QR_CODE_SIZE) / 2, y, QR_CODE_SIZE, QR_CODE_SIZE);
    QrUtils::drawQrCode(renderer, qrBounds, verificationUriComplete);
  } else if (state == SUCCESS) {
    const int top = (pageHeight - lineH) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BF_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
  } else if (state == FAILED) {
    const int top = (pageHeight - lineH) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BF_AUTH_FAILED_TITLE), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const auto errorLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), messageWidth, 3);
    int messageY = top + lineH + 10;
    for (const auto& line : errorLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineH + 4;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
