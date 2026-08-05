#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "activities/Activity.h"

/**
 * Syncs reading progress with BookFusion for the current book.
 *
 * Flow: connect WiFi -> fetch remote percentage -> compare with local
 * percentage -> let the user apply the remote value or upload the local one
 * (or skip the choice entirely if only one side has progress).
 *
 * BookFusion's protocol is percentage-only (no chapter/xpath granularity),
 * so "apply" lands at the start of the resolved chapter rather than an exact
 * page -- consistent with what BookFusion itself actually stores. This is
 * self-contained: it only uses the generic Epub/EpubReaderUtils APIs, not
 * anything from lib/KOReaderSync/.
 */
class BookFusionSyncActivity final : public Activity {
 public:
  explicit BookFusionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath)
      : Activity("BookFusionSync", renderer, mappedInput), epubPath(std::move(epubPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    SYNC_COMPLETE,
    SYNC_FAILED,
    NO_BOOK_LINK,
  };

  std::string epubPath;
  std::shared_ptr<Epub> epub;  // null until lazy-loaded in ensureEpubLoaded()
  uint32_t bookId = 0;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  float localPercent = 0.0f;
  float remotePercent = 0.0f;
  bool hasRemoteProgress = false;

  // Selection in result screen (0 = Apply remote, 1 = Upload local)
  int selectedOption = 0;

  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void applyRemoteProgress();
  void uploadLocalProgress();
  bool ensureEpubLoaded();
  void returnToReader();
  void markAutoReturn();
};
