#include "BookFusionSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cmath>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace {
// Same-progress tolerance used to skip the Apply/Upload prompt when both
// sides already agree.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;
}  // namespace

bool BookFusionSyncActivity::ensureEpubLoaded() {
  if (epub) return true;
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS needed for progress mapping.
  if (!epub->load(false, true)) {
    LOG_ERR("BFSync", "Failed to load epub for progress mapping");
    epub.reset();
    return false;
  }
  return true;
}

void BookFusionSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void BookFusionSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void BookFusionSyncActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  bookId = BookFusionBookIdStore::loadBookId(epubPath);
  if (bookId == 0) {
    state = NO_BOOK_LINK;
    errorMessage = tr(STR_BF_NOT_LINKED_MSG);
    requestUpdate();
    return;
  }
  if (BookFusionSyncClient::getBearerToken().empty()) {
    state = NO_BOOK_LINK;
    errorMessage = tr(STR_BF_NO_TOKEN_MSG);
    requestUpdate();
    return;
  }

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionSyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

void BookFusionSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_BF_SYNCING);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFSync", "Syncing screen could not be rendered before request");
    requestUpdate(true);
  }

  performSync();
}

void BookFusionSyncActivity::performSync() {
  if (!ensureEpubLoaded()) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = tr(STR_BF_EPUB_LOAD_FAILED);
    requestUpdate();
    return;
  }

  EpubReaderUtils::Progress localProgress;
  if (EpubReaderUtils::loadProgress(*epub, localProgress, "BFSync") && localProgress.hasPageCount &&
      localProgress.pageCount > 0) {
    const float chapterProgress =
        static_cast<float>(localProgress.pageNumber + 1) / static_cast<float>(localProgress.pageCount);
    localPercent = epub->calculateProgress(localProgress.spineIndex, chapterProgress);
  } else {
    localPercent = 0.0f;
  }

  BookFusionProgress remote;
  const auto result = BookFusionSyncClient::getProgress(bookId, remote);
  hasRemoteProgress = result == BookFusionSyncClient::OK;
  if (hasRemoteProgress) {
    remotePercent = remote.percentage;
  } else if (result != BookFusionSyncClient::NOT_FOUND) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = BookFusionSyncClient::errorString(result);
    requestUpdate();
    return;
  }

  if (!hasRemoteProgress) {
    // Nothing to compare against; just upload local progress.
    uploadLocalProgress();
    return;
  }

  if (std::fabs(localPercent - remotePercent) < SAME_PROGRESS_EPSILON) {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
    statusMessage = tr(STR_ALREADY_SYNCED);
    markAutoReturn();
    requestUpdate();
    return;
  }

  RenderLock lock(*this);
  state = SHOWING_RESULT;
  selectedOption = remotePercent > localPercent ? 0 : 1;
  requestUpdate();
}

void BookFusionSyncActivity::applyRemoteProgress() {
  {
    RenderLock lock(*this);
    state = UPLOADING;  // reuses the "busy" state visually; no separate APPLYING state needed
    statusMessage = tr(STR_BF_APPLYING);
  }
  requestUpdate(true);

  const int percentInt = std::max(0, std::min(100, static_cast<int>(remotePercent * 100.0f + 0.5f)));
  int spineIndex = 0;
  float spineProgress = 0.0f;
  bool saved = false;
  if (epub->resolveLocationPercentToSpineProgress(percentInt, spineIndex, spineProgress)) {
    // BookFusion has no chapter/page granularity, so land at the start of the
    // resolved chapter rather than fabricating an exact page.
    saved = EpubReaderUtils::saveProgress(*epub, spineIndex, 0, 1);
  }

  if (!saved) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = tr(STR_SAVE_PROGRESS_FAILED);
    requestUpdate();
    return;
  }

  RecentBookProgress::saveCachedEpubPercent(epub->getCachePath(), remotePercent * 100.0f);
  returnToReader();
}

void BookFusionSyncActivity::uploadLocalProgress() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_BF_UPLOADING);
  }
  requestUpdate(true);

  BookFusionProgress progress;
  progress.bookId = bookId;
  progress.percentage = localPercent;
  const auto result = BookFusionSyncClient::updateProgress(progress);

  if (result != BookFusionSyncClient::OK) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = BookFusionSyncClient::errorString(result);
    requestUpdate();
    return;
  }

  RenderLock lock(*this);
  state = SYNC_COMPLETE;
  statusMessage = tr(STR_BF_UPLOAD_COMPLETE);
  markAutoReturn();
  requestUpdate();
}

void BookFusionSyncActivity::loop() {
  if (state == WIFI_SELECTION) return;

  if (state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
    }
    return;
  }

  if (state == NO_BOOK_LINK || state == SYNC_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = selectedOption == 0 ? 1 : 0;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        applyRemoteProgress();
      } else {
        uploadLocalProgress();
      }
    }
    return;
  }
}

void BookFusionSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  GUI.drawHeader(renderer, header, tr(STR_BF_SYNC));

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineH) / 2;

  switch (state) {
    case CONNECTING:
    case SYNCING:
    case UPLOADING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
      break;
    case SHOWING_RESULT: {
      char localBuf[32];
      char remoteBuf[32];
      snprintf(localBuf, sizeof(localBuf), tr(STR_BF_LOCAL_PROGRESS_FORMAT), localPercent * 100.0f);
      snprintf(remoteBuf, sizeof(remoteBuf), tr(STR_BF_REMOTE_PROGRESS_FORMAT), remotePercent * 100.0f);
      renderer.drawCenteredText(UI_10_FONT_ID, top - lineH - 8, localBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, top, remoteBuf);
      const char* optionText = selectedOption == 0 ? tr(STR_BF_APPLY_REMOTE) : tr(STR_BF_UPLOAD_LOCAL);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 8, optionText, true, EpdFontFamily::BOLD);
      break;
    }
    case SYNC_COMPLETE:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
      break;
    case NO_BOOK_LINK:
    case SYNC_FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, top, errorMessage.c_str());
      break;
    default:
      break;
  }

  MappedInputManager::Labels labels;
  if (state == SHOWING_RESULT) {
    labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_BF_SWITCH), "");
  } else {
    labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
