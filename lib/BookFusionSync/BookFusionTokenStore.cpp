#include "BookFusionTokenStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

void BookFusionTokenStore::toJson(JsonDocument& doc) const {
  doc["accessToken_obf"] = obfuscation::obfuscateToBase64(accessToken);
  doc["refreshToken_obf"] = obfuscation::obfuscateToBase64(refreshToken);
}

bool BookFusionTokenStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;

  obfuscation::DecodeStatus accessStatus = obfuscation::DecodeStatus::INVALID;
  accessToken = obfuscation::deobfuscateFromBase64(doc["accessToken_obf"] | "", &accessStatus);
  if (accessStatus == obfuscation::DecodeStatus::LEGACY && !accessToken.empty()) {
    needsResave = true;
  }
  if (accessStatus == obfuscation::DecodeStatus::INVALID && !accessToken.empty()) {
    LOG_ERR("BFS", "Ignoring unreadable BookFusion access token");
    accessToken.clear();
  }

  obfuscation::DecodeStatus refreshStatus = obfuscation::DecodeStatus::INVALID;
  refreshToken = obfuscation::deobfuscateFromBase64(doc["refreshToken_obf"] | "", &refreshStatus);
  if (refreshStatus == obfuscation::DecodeStatus::LEGACY && !refreshToken.empty()) {
    needsResave = true;
  }
  if (refreshStatus == obfuscation::DecodeStatus::INVALID && !refreshToken.empty()) {
    LOG_ERR("BFS", "Ignoring unreadable BookFusion refresh token");
    refreshToken.clear();
  }

  if (needsResave) {
    LOG_DBG("BFS", "Resaving BookFusion tokens to update format");
    requestResave();
  }

  return true;
}

void BookFusionTokenStore::setTokens(const std::string& access, const std::string& refresh) {
  accessToken = access;
  refreshToken = refresh;
}

bool BookFusionTokenStore::hasToken() const { return !accessToken.empty(); }

void BookFusionTokenStore::clearTokens() {
  accessToken.clear();
  refreshToken.clear();
  saveToFile();
}
