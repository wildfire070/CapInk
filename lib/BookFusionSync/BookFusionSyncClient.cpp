#include "BookFusionSyncClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <I18n.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#endif

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "BookFusionTokenStore.h"

int BookFusionSyncClient::lastHttpCode = 0;
int BookFusionSyncClient::lastTransportError = 0;

namespace {
constexpr char BASE_URL[] = "https://www.bookfusion.com";

// BookFusion's device-code endpoint is the same one KOReader's official
// BookFusion cloud-storage plugin uses; "koreader" is that plugin's public
// OAuth client_id, not a secret. CrossInk already speaks the KOReader/
// crosspoint-sync protocol family (see lib/KOReaderSync/), so identifying as
// this client for BookFusion's device flow is consistent with what CrossInk
// already is, not spoofing another app.
//
// NOTE: this endpoint/field shape was not independently verified against
// BookFusion's live API docs -- it comes from a now-stale reference fork
// (see plan doc) and matches the standard RFC 8628 device-flow shape. Sanity
// check against BookFusion's current docs before shipping.
constexpr char CLIENT_ID[] = "koreader";
constexpr char DEVICE_CODE_GRANT_TYPE[] = "urn:ietf:params:oauth:grant-type:device_code";
constexpr char API_ACCEPT[] = "application/json; api_version=10";

const char* classifyJsonBody(const char* body) {
  if (!body || body[0] == '\0') return "empty response";
  const char* cursor = body;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
  if (*cursor == '\0') return "blank response";
  if (*cursor == '<') return "HTML response";
  if (*cursor != '{' && *cursor != '[') return "non-JSON response";
  return "malformed JSON";
}

void logJsonParseFailure(const char* context, DeserializationError error, const char* body) {
  char preview[97];
  size_t i = 0;
  if (body) {
    for (; i < sizeof(preview) - 1 && body[i] != '\0'; i++) {
      const char c = body[i];
      preview[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
  }
  preview[i] = '\0';
  LOG_ERR("BFS", "%s JSON parse failed: %s (%s, preview=\"%s\")", context, error.c_str(), classifyJsonBody(body),
          preview);
}

// Same heap floor KOReaderSyncClient uses before a TLS handshake -- both
// clients run over the same wolfSSL transport on-device.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("BFS", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}

#ifdef SIMULATOR
void addAuthHeaders(HTTPClient& http) {
  const std::string bearer = "Bearer " + BOOKFUSION_STORE.getAccessToken();
  http.addHeader("Authorization", bearer.c_str());
  http.addHeader("Accept", API_ACCEPT);
}
#else
void addAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Authorization", std::string("Bearer ") + BOOKFUSION_STORE.getAccessToken());
  http.addHeader("Accept", API_ACCEPT);
}
#endif
}  // namespace

BookFusionSyncClient::Error BookFusionSyncClient::startDeviceAuth(BookFusionDeviceAuth& outAuth) {
  lastHttpCode = 0;
  lastTransportError = 0;

  const std::string url = std::string(BASE_URL) + "/api/user/auth/device";
  LOG_DBG("BFS", "Requesting device code: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["client_id"] = CLIENT_ID;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "startDeviceAuth response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("startDeviceAuth", error, responseBody.c_str());
    return JSON_ERROR;
  }

  outAuth.deviceCode = doc["device_code"] | "";
  outAuth.userCode = doc["user_code"] | "";
  outAuth.verificationUri = doc["verification_uri"] | "";
  outAuth.verificationUriComplete = doc["verification_uri_complete"] | "";
  outAuth.interval = doc["interval"] | 5;
  outAuth.expiresIn = doc["expires_in"] | 600;

  if (outAuth.deviceCode.empty() || outAuth.userCode.empty()) {
    LOG_ERR("BFS", "startDeviceAuth: missing device_code/user_code in response");
    return JSON_ERROR;
  }

  LOG_DBG("BFS", "Device code received: user_code=%s, interval=%ds, expires_in=%ds", outAuth.userCode.c_str(),
          outAuth.interval, outAuth.expiresIn);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::pollForToken(const std::string& deviceCode) {
  lastHttpCode = 0;
  lastTransportError = 0;

  const std::string url = std::string(BASE_URL) + "/api/user/auth/token";
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["grant_type"] = DEVICE_CODE_GRANT_TYPE;
  reqDoc["client_id"] = CLIENT_ID;
  reqDoc["device_code"] = deviceCode;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "pollForToken response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("pollForToken", error, responseBody.c_str());
    return JSON_ERROR;
  }

  if (httpCode == 200) {
    const char* token = doc["access_token"] | "";
    if (token[0] == '\0') return JSON_ERROR;
    const char* refresh = doc["refresh_token"] | "";
    BOOKFUSION_STORE.setTokens(token, refresh);
    BOOKFUSION_STORE.saveToFile();
    LOG_DBG("BFS", "Token received and saved");
    return OK;
  }

  const char* errCode = doc["error"] | "";
  LOG_DBG("BFS", "pollForToken error: %s", errCode);

  if (std::strcmp(errCode, "authorization_pending") == 0) return AUTH_PENDING;
  if (std::strcmp(errCode, "slow_down") == 0) return SLOW_DOWN;
  if (std::strcmp(errCode, "expired_token") == 0) return EXPIRED;
  if (std::strcmp(errCode, "access_denied") == 0) return AUTH_FAILED;
  // BookFusion has been observed returning "invalid_grant" (HTTP 400) while
  // authorization is still pending, which is non-standard but matches what
  // KOReader's own BookFusion plugin tolerates -- treat any unrecognized
  // error the same way rather than giving up early.
  return AUTH_PENDING;
}

BookFusionSyncClient::Error BookFusionSyncClient::searchBooks(int page, const char* list, BookFusionSearchResult& out) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  const std::string url = std::string(BASE_URL) + "/api/user/books/search";
  LOG_DBG("BFS", "searchBooks page=%d (heap: %u)", page, (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Request one extra book to detect hasMore without relying on headers; see
  // BOOKFUSION_BOOKS_PER_PAGE for the heap-budget reasoning.
  JsonDocument reqDoc;
  reqDoc["page"] = page;
  reqDoc["per_page"] = BOOKFUSION_BOOKS_PER_PAGE + 1;
  reqDoc["sort"] = "added_at-desc";
  if (list != nullptr) reqDoc["list"] = list;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "searchBooks page=%d response: %d", page, httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode != 200) return SERVER_ERROR;

  // Discard every field but the ones we display; BookFusion book objects
  // carry ~20 fields (cover URLs, descriptions, etc.) that would otherwise
  // multiply JsonDocument heap use several times over for no benefit here.
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["authors"][0]["name"] = true;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str(), DeserializationOption::Filter(filter));
  if (error) {
    logJsonParseFailure("searchBooks", error, responseBody.c_str());
    return JSON_ERROR;
  }
  if (!doc.is<JsonArray>()) {
    LOG_ERR("BFS", "searchBooks: expected a JSON array");
    return JSON_ERROR;
  }

  out.books.clear();
  out.books.reserve(BOOKFUSION_BOOKS_PER_PAGE);
  out.currentPage = page;
  out.hasMore = false;

  for (JsonObject book : doc.as<JsonArray>()) {
    if (static_cast<int>(out.books.size()) >= BOOKFUSION_BOOKS_PER_PAGE) {
      out.hasMore = true;
      break;
    }

    BookFusionBook b;
    b.bookId = book["id"] | (uint32_t)0;
    if (b.bookId == 0) continue;
    b.title = book["title"] | "Untitled";

    std::string authors;
    for (JsonObject author : book["authors"].as<JsonArray>()) {
      const char* name = author["name"] | "";
      if (name[0] == '\0') continue;
      if (!authors.empty()) authors += ", ";
      authors += name;
    }
    b.author = std::move(authors);

    out.books.push_back(std::move(b));
  }

  LOG_DBG("BFS", "searchBooks: %zu books on page %d, hasMore=%d", out.books.size(), page, out.hasMore);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::getProgress(uint32_t bookId, BookFusionProgress& outProgress) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  char urlBuf[128];
  snprintf(urlBuf, sizeof(urlBuf), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  const std::string url = urlBuf;
  LOG_DBG("BFS", "getProgress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "getProgress response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("getProgress", error, responseBody.c_str());
    return JSON_ERROR;
  }

  outProgress.bookId = bookId;
  outProgress.percentage = doc["percentage"] | 0.0f;
  outProgress.timestamp = doc["updated_at"] | (int64_t)0;

  LOG_DBG("BFS", "Remote progress: %.2f%%", outProgress.percentage * 100);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::updateProgress(const BookFusionProgress& progress) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  char urlBuf[128];
  snprintf(urlBuf, sizeof(urlBuf), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)progress.bookId);
  const std::string url = urlBuf;
  LOG_DBG("BFS", "updateProgress: %s (%.2f%%)", url.c_str(), progress.percentage * 100);
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["percentage"] = progress.percentage;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();
#endif

  LOG_DBG("BFS", "updateProgress response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 200 || httpCode == 201) return OK;
  return SERVER_ERROR;
}

BookFusionSyncClient::Error BookFusionSyncClient::getDownloadUrl(uint32_t bookId, std::string& outUrl) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  char urlBuf[128];
  snprintf(urlBuf, sizeof(urlBuf), "%s/api/user/books/%lu/download", BASE_URL, (unsigned long)bookId);
  const std::string url = urlBuf;
  LOG_DBG("BFS", "getDownloadUrl: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST("{}");
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", "{}");
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "getDownloadUrl book=%lu response: %d", (unsigned long)bookId, httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 403 || httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("getDownloadUrl", error, responseBody.c_str());
    return JSON_ERROR;
  }

  outUrl = doc["url"] | "";
  if (outUrl.empty()) {
    LOG_ERR("BFS", "getDownloadUrl: missing url field");
    return JSON_ERROR;
  }
  return OK;
}

std::string BookFusionSyncClient::getBearerToken() { return BOOKFUSION_STORE.getAccessToken(); }

std::string BookFusionSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return tr(STR_BF_NO_TOKEN_MSG);
    case NETWORK_ERROR:
      return tr(STR_BF_NETWORK_ERROR);
    case AUTH_FAILED:
      return tr(STR_BF_AUTH_REJECTED);
    case AUTH_PENDING:
      return tr(STR_BF_AUTH_PENDING);
    case SLOW_DOWN:
      return tr(STR_BF_AUTH_PENDING);
    case EXPIRED:
      return tr(STR_BF_AUTH_EXPIRED);
    case SERVER_ERROR:
      if (lastHttpCode > 0) {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), tr(STR_BF_HTTP_STATUS_FORMAT), lastHttpCode);
        return std::string(buffer);
      }
      return tr(STR_BF_SERVER_ERROR);
    case JSON_ERROR:
      return tr(STR_BF_BAD_RESPONSE);
    case NOT_FOUND:
      return tr(STR_BF_NOT_FOUND);
    case LOW_MEMORY:
      return tr(STR_BF_LOW_MEMORY);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
