#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * BookFusion account linking via OAuth 2.0 Device Authorization Grant
 * (RFC 8628): shows a short code and a QR code linking to the verification
 * page, then polls in loop() via millis() until the user approves it on
 * another device -- no FreeRTOS task needed, matching
 * BookFusionSyncClient::pollForToken()'s one-shot-per-call design.
 */
class BookFusionAuthActivity final : public Activity {
 public:
  explicit BookFusionAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionAuth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == REQUESTING_CODE || state == WAITING_FOR_USER || state == POLLING;
  }

 private:
  enum State { WIFI_SELECTION, CONNECTING, REQUESTING_CODE, WAITING_FOR_USER, POLLING, SUCCESS, FAILED };

  static constexpr int MAX_NETWORK_RETRIES = 3;

  State state = WIFI_SELECTION;
  std::string errorMessage;

  std::string deviceCode;  // Internal only -- never shown to the user.
  std::string userCode;
  std::string verificationUri;          // Displayed as text.
  std::string verificationUriComplete;  // QR payload; falls back to verificationUri if the server omitted it.
  int pollIntervalSec = 5;
  unsigned long pollExpireAt = 0;      // millis() deadline
  unsigned long nextPollAt = 0;        // millis() of next poll attempt
  unsigned long lastTimerRefresh = 0;  // Throttles e-ink redraws of the countdown.
  int networkRetries = 0;

  void onWifiSelectionComplete(bool success);
  void requestCode();
  void doPoll();
};
