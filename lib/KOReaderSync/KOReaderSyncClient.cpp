#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Epub.h>

#include <cmath>
#include <cstdlib>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <I18n.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <HTTPClient.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#include <base64.h>
#endif

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include "KOReaderCredentialStore.h"
#include "Memory.h"

#ifndef SIMULATOR
#include <esp_mac.h>
#endif

#if !defined(SIMULATOR) && defined(FREEINK_WOLFSSL_DEBUG)
// With FREEINK_WOLFSSL_DEBUG, wolfSSL is built with DEBUG_WOLFSSL, whose Arduino
// backend expects the app to provide this print hook (the stock definition lives
// in <wolfssl.h>, which no translation unit here includes). Route it through the firmware logger, same
// as upstream CrossPoint's HttpDownloader.cpp.
extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#endif

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastTransportError = 0;

namespace {

constexpr bool isSuccessfulHttpCode(int httpCode) { return httpCode >= 200 && httpCode < 300; }

std::string formatHttpStatusMessage(int httpCode) {
  char buffer[96];
  snprintf(buffer, sizeof(buffer), tr(STR_KOREADER_SYNC_HTTP_STATUS_FORMAT), httpCode);
  return std::string(buffer);
}

std::string networkErrorMessage() {
#ifdef SIMULATOR
  switch (KOReaderSyncClient::lastTransportError) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
    case HTTPC_ERROR_NOT_CONNECTED:
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return tr(STR_KOREADER_SYNC_NETWORK_REFUSED);
    case HTTPC_ERROR_CONNECTION_LOST:
    case HTTPC_ERROR_READ_TIMEOUT:
      return tr(STR_KOREADER_SYNC_NETWORK_TIMEOUT);
    default:
      return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
  }
#else
  // SecureHttpClient reports transport failures as a bare -1 (no granular
  // error codes), so there is nothing finer to translate on device.
  return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
#endif
}

const char* classifyJsonBody(const char* body) {
  if (!body || body[0] == '\0') return "empty response";

  const char* cursor = body;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }

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

  LOG_ERR("KOSync", "%s JSON parse failed: %s (%s, preview=\"%s\")", context, error.c_str(), classifyJsonBody(body),
          preview);
}

KOReaderSyncClient::Error validateAuthResponse(const char* body) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body ? body : "");
  if (error) {
    logJsonParseFailure("Auth", error, body);
    return KOReaderSyncClient::JSON_ERROR;
  }

  if (!doc.is<JsonObject>()) {
    LOG_ERR("KOSync", "Auth response was not a JSON object");
    return KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  }

  const char* authorized = doc["authorized"] | nullptr;
  if (authorized && std::strcmp(authorized, "OK") != 0) {
    LOG_ERR("KOSync", "Auth response explicitly denied authorization");
    return KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  }

  return KOReaderSyncClient::OK;
}

// KOSync's TLS-1.3 servers can't be reached through the precompiled system
// mbedTLS (TLS 1.3 is stubbed out), so requests run over wolfSSL via
// SecureHttpClient. The handshake still needs working heap; gate on it. wolfSSL's
// footprint is smaller than mbedTLS's old ~48KB peak, but keep conservative
// floors for total free heap and the largest contiguous block.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;
// Authentication returns a small JSON object. Cap unexpected HTML/error pages
// before they can exhaust the C3 heap while the TLS connection is still open.
constexpr size_t MAX_AUTH_RESPONSE_BYTES = 4096;

#ifdef SIMULATOR
void addAuthHeaders(HTTPClient& http) {
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
#else
// Apply the shared KOSync auth headers after begin(). x-auth-* is the native
// KOSync scheme; Basic auth is added for Calibre-Web-Automated compatibility.
void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
}
#endif

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode == 200) {
    if (http.getSize() > static_cast<int>(MAX_AUTH_RESPONSE_BYTES)) {
      LOG_ERR("KOSync", "Auth response exceeded %u bytes (HTTP %d)", static_cast<unsigned>(MAX_AUTH_RESPONSE_BYTES),
              httpCode);
      http.end();
      return INVALID_AUTH_RESPONSE;
    }
    String responseBody = http.getString();
    http.end();
    return validateAuthResponse(responseBody.c_str());
  }

  http.end();

  // KOSync-compatible implementations can use other successful 2xx codes.
  if (isSuccessfulHttpCode(httpCode)) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  std::unique_ptr<char[]> responseBody;
  size_t responseSize = 0;
  bool responseTooLarge = false;
  bool responseOutOfMemory = false;
  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    // Only HTTP 200 needs its JSON body; other statuses are handled below.
    if (http.getStatus() != 200) return true;
    if (len > MAX_AUTH_RESPONSE_BYTES - responseSize) {
      responseTooLarge = true;
      return false;
    }
    if (!responseBody) {
      responseBody = makeUniqueNoThrow<char[]>(MAX_AUTH_RESPONSE_BYTES + 1);
      if (!responseBody) {
        responseOutOfMemory = true;
        return false;
      }
    }
    std::memcpy(responseBody.get() + responseSize, data, len);
    responseSize += len;
    responseBody[responseSize] = '\0';
    return true;
  });
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (responseTooLarge) {
    LOG_ERR("KOSync", "Auth response exceeded %u bytes (HTTP %d)", static_cast<unsigned>(MAX_AUTH_RESPONSE_BYTES),
            httpCode);
    http.end();
    return INVALID_AUTH_RESPONSE;
  }
  if (responseOutOfMemory) {
    LOG_ERR("KOSync", "Not enough memory for auth response (HTTP %d)", httpCode);
    http.end();
    return LOW_MEMORY;
  }
  if (httpCode == 200) {
    if (!http.responseComplete()) {
      LOG_ERR("KOSync", "Auth response incomplete");
      http.end();
      return NETWORK_ERROR;
    }
    const Error result = validateAuthResponse(responseBody ? responseBody.get() : "");
    http.end();
    return result;
  }
  http.end();
  // KOSync-compatible implementations use different successful 2xx codes.
  // Keep CrossInk's validation of the reference server's 200 JSON response.
  if (isSuccessfulHttpCode(httpCode)) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Create user response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (isSuccessfulHttpCode(httpCode)) return OK;  // 2xx: created (see #2876)
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url =
      KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash + "?position_kinds=locator,percentage";
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  // 204 means this document has no stored progress. Some KOSync-compatible
  // servers use it where the reference server returns 200 with an empty body.
  if (httpCode == 204) {
    http.end();
    return NOT_FOUND;
  }

  if (isSuccessfulHttpCode(httpCode)) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      logJsonParseFailure("Get progress", error, responseBody.c_str());
      return JSON_ERROR;
    }

    if (doc["progress"].isNull() && !(doc["percentage"].is<float>() && std::isfinite(doc["percentage"].as<float>()) &&
                                      doc["percentage"].as<float>() >= 0 && doc["percentage"].as<float>() <= 1)) {
      LOG_DBG("KOSync", "No stored progress in successful response");
      return NOT_FOUND;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"] | "";
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }

  // 204 = success with no stored progress for this document (Spring-style
  // KOSync implementations; the reference server answers 200 with an empty
  // object instead). Map it to the same graceful no-remote-progress path as
  // 404 rather than falling through to SERVER_ERROR — see issue #2876.
  if (httpCode == 204) {
    http.end();
    return NOT_FOUND;
  }

  if (isSuccessfulHttpCode(httpCode)) {
    const std::string& body = http.getString();
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);

    if (error) {
      logJsonParseFailure("Get progress", error, body.c_str());
      http.end();
      return JSON_ERROR;
    }

    if (doc["progress"].isNull() && !(doc["percentage"].is<float>() && std::isfinite(doc["percentage"].as<float>()) &&
                                      doc["percentage"].as<float>() >= 0 && doc["percentage"].as<float>() <= 1)) {
      http.end();
      LOG_DBG("KOSync", "No stored progress in successful response");
      return NOT_FOUND;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"] | "";
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    outProgress.position.reset();
    if (KOREADER_STORE.supportsRichProgress()) {
      const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
      if (!pos.isNull()) {
        KOReaderRichPosition rich;
        rich.pctQ = pos["pctQ"].as<uint32_t>();
        rich.spineIndex = pos["spine"].as<uint16_t>();
        rich.pageNumber = pos["page"].as<uint16_t>();
        const uint16_t pages = pos["pages"].as<uint16_t>();
        rich.totalPages = pages > 0 ? pages : 1;
        const uint16_t para = pos["para"].as<uint16_t>();
        if (para > 0) rich.paragraphIndex = para;
        rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
        LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                rich.totalPages, para);
        outProgress.position = std::move(rich);
      }
    }

    http.end();
    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
#endif
}

namespace {
// One progress item, shared by the single and batch PUTs.
void fillProgressJson(JsonObject item, const KOReaderProgress& progress) {
  item["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = item["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
    if (KOREADER_STORE.supportsExtendedMetadata()) {
      if (!progress.metadata->isbn.empty()) meta["isbn"] = progress.metadata->isbn;
      if (!progress.metadata->asin.empty()) meta["asin"] = progress.metadata->asin;
      if (!progress.metadata->series.empty()) meta["series"] = progress.metadata->series;
      if (progress.metadata->seriesIndex) meta["series_index"] = *progress.metadata->seriesIndex;
    }
  }
  item["progress"] = progress.progress;
  item["percentage"] = progress.percentage;
  item["device"] = progress.device;
  // Before per-reader ids every CrossInk reader sent "crossink-device"; the
  // server compares a reader's first write against that shared row. Only a
  // server confirmed to be CrossPoint Sync (default or self-hosted) gets the
  // per-reader id, which its change feed needs: other KOSync servers keep the
  // shared id and never see the reader's hardware address.
  char readerIdBuffer[32];
  if (KOREADER_STORE.getServerSupport() != SyncServerSupport::SUPPORTED ||
      !KOReaderSyncClient::readerId(readerIdBuffer, sizeof(readerIdBuffer))) {
    snprintf(readerIdBuffer, sizeof(readerIdBuffer), "crossink-device");
  }
  item["device_id"] = static_cast<const char*>(readerIdBuffer);
  if (progress.position.has_value() && KOREADER_STORE.supportsRichProgress()) {
    // Rich positions are opt-in by profile; standard KOSync never receives them.
    const auto& p = *progress.position;
    auto pos = item["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }
}

// PUT `body` as JSON with the account headers; returns the HTTP code (<= 0 on transport errors).
int putJson(const std::string& url, const std::string& body) {
#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.PUT(body.c_str());
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    KOReaderSyncClient::lastHttpCode = -1;
    KOReaderSyncClient::lastTransportError = -1;
    return -1;
  }
  applyAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PUT", body);
  http.end();
#endif
  KOReaderSyncClient::lastHttpCode = httpCode;
  KOReaderSyncClient::lastTransportError = (httpCode < 0) ? httpCode : 0;
  return httpCode;
}

}  // namespace

namespace {
KOReaderSyncClient::Error putResult(const int httpCode) {
  if (httpCode <= 0) return KOReaderSyncClient::NETWORK_ERROR;
  // Any 2xx accepts the progress. The reference kosync server answers 200,
  // but Spring-based KOSync implementations (BookLore/grimmory) answer a PUT
  // with the idiomatic 201/204, which used to land in SERVER_ERROR and made
  // every sync against them fail after a successful pull — issue #2876.
  if (isSuccessfulHttpCode(httpCode)) return KOReaderSyncClient::OK;
  if (httpCode == 401) return KOReaderSyncClient::AUTH_FAILED;
  return KOReaderSyncClient::SERVER_ERROR;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  std::string body;
  {
    JsonDocument doc;
    fillProgressJson(doc.to<JsonObject>(), progress);
    serializeJson(doc, body);
  }
  const int httpCode = putJson(url, body);
  LOG_DBG("KOSync", "Update progress response: %d", httpCode);
  return putResult(httpCode);
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgressBatch(std::vector<KOReaderProgress>& items) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (items.empty()) return OK;
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/progress/batch";
  // Serialize, then release the items and the JSON tree before the TLS
  // connection allocates its buffers: only the body is alive during the request.
  std::string body;
  {
    JsonDocument doc;
    auto array = doc["items"].to<JsonArray>();
    for (const auto& progress : items) fillProgressJson(array.add<JsonObject>(), progress);
    serializeJson(doc, body);
  }
  const size_t count = items.size();
  std::vector<KOReaderProgress>().swap(items);
  LOG_DBG("KOSync", "Batch progress: %u books, %u bytes (heap: %u)", static_cast<unsigned>(count),
          static_cast<unsigned>(body.size()), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;
  const int httpCode = putJson(url, body);
  LOG_DBG("KOSync", "Batch progress response: %d", httpCode);
  // Stock KOSync and older CrossPoint Sync servers: one PUT per book instead.
  if (httpCode == 404) return BATCH_UNSUPPORTED;
  return putResult(httpCode);
}

namespace {
// The server keeps pages under 8 KB; anything much larger is not the feed.
constexpr size_t MAX_CHANGES_RESPONSE_BYTES = 16 * 1024;

KOReaderSyncClient::Error parseChangesPage(const char* body, const size_t length, KOReaderSyncClient::ChangesPage& page,
                                           const KOReaderSyncClient::ChangeCallback callback, void* context) {
  // Keep only the fields the pull uses; everything else in the body is skipped.
  JsonDocument filter;
  filter["cursor"] = true;
  filter["more"] = true;
  filter["devices"] = true;
  filter["document"] = true;
  if (length > MAX_CHANGES_RESPONSE_BYTES) {
    // The server always sends at least one change; one too large to handle
    // here is skipped by moving the cursor past it rather than stopping the feed.
    LOG_ERR("KOSync", "Change feed page too large (%u bytes); skipping it", static_cast<unsigned>(length));
    JsonDocument cursorOnly;
    if (deserializeJson(cursorOnly, body, length, DeserializationOption::Filter(filter)) ||
        !cursorOnly["cursor"].is<uint32_t>())
      return KOReaderSyncClient::JSON_ERROR;
    page.cursor = cursorOnly["cursor"].as<uint32_t>();
    page.more = cursorOnly["more"].as<bool>();
    page.count = 0;
    return KOReaderSyncClient::OK;
  }
  JsonObject item = filter["changes"][0].to<JsonObject>();
  for (const char* key : {"document", "aliases", "percentage", "progress", "device", "timestamp"}) item[key] = true;
  for (const char* key : {"pctQ", "spine", "page", "pages", "para", "xpath"}) item["position"][key] = true;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body, length, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("KOSync", "Change feed JSON error: %s", error.c_str());
    return KOReaderSyncClient::JSON_ERROR;
  }
  // An older CrossPoint Sync reads "changes" as a document hash and answers 200
  // with that document's (empty) device list: no feed there.
  if (!doc["cursor"].is<uint32_t>()) {
    return doc["devices"].is<JsonArrayConst>() || doc["document"].is<const char*>()
               ? KOReaderSyncClient::FEED_UNSUPPORTED
               : KOReaderSyncClient::JSON_ERROR;
  }
  page.cursor = doc["cursor"].as<uint32_t>();
  page.more = doc["more"].as<bool>();
  page.count = 0;
  const bool richPositions = KOREADER_STORE.supportsRichProgress();
  // ~300 bytes with its aliases: one allocation per page instead of stack
  // space on a path that runs with TLS up.
  auto changeStorage = makeUniqueNoThrow<KOReaderProgressChange>();
  if (!changeStorage) {
    LOG_ERR("KOSync", "Cannot allocate change feed entry");
    return KOReaderSyncClient::LOW_MEMORY;
  }
  KOReaderProgressChange& change = *changeStorage;
  for (JsonObjectConst entry : doc["changes"].as<JsonArrayConst>()) {
    const char* document = entry["document"].as<const char*>();
    const char* progress = entry["progress"].as<const char*>();
    if (!document || !progress) continue;
    change = {};
    change.document = document;
    for (JsonVariantConst alias : entry["aliases"].as<JsonArrayConst>()) {
      if (change.aliasCount >= KOReaderProgressChange::MAX_ALIASES) break;
      if (const char* value = alias.as<const char*>()) change.aliases[change.aliasCount++] = value;
    }
    change.percentage = entry["percentage"].as<float>();
    change.progress = progress;
    change.device = entry["device"].as<const char*>() ? entry["device"].as<const char*>() : "";
    change.timestamp = entry["timestamp"].as<int64_t>();
    const JsonObjectConst pos = entry["position"].as<JsonObjectConst>();
    if (richPositions && !pos.isNull()) {
      KOReaderRichPosition rich;
      rich.pctQ = pos["pctQ"].as<uint32_t>();
      rich.spineIndex = pos["spine"].as<uint16_t>();
      rich.pageNumber = pos["page"].as<uint16_t>();
      const uint16_t pages = pos["pages"].as<uint16_t>();
      rich.totalPages = pages > 0 ? pages : 1;
      const uint16_t para = pos["para"].as<uint16_t>();
      if (para > 0) rich.paragraphIndex = para;
      rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
      change.position = std::move(rich);
    }
    ++page.count;
    if (callback && !callback(change, context)) break;
  }
  return KOReaderSyncClient::OK;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::getProgressChanges(const uint32_t since, const uint8_t limit,
                                                                 ChangesPage& page, const ChangeCallback callback,
                                                                 void* context) {
  lastHttpCode = 0;
  lastTransportError = 0;
  page = {};
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  char reader[32];
  if (!readerId(reader, sizeof(reader))) snprintf(reader, sizeof(reader), "crossink-device");
  char query[96];
  snprintf(query, sizeof(query), "/api/v1/progress/changes?since=%lu&limit=%u&device=%s",
           static_cast<unsigned long>(since), static_cast<unsigned>(limit), reader);
  const std::string url = KOREADER_STORE.getBaseUrl() + query;
  LOG_DBG("KOSync", "Getting changes since %lu (heap: %u)", static_cast<unsigned long>(since),
          static_cast<unsigned>(ESP.getFreeHeap()));
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  if (!isSuccessfulHttpCode(httpCode)) {
    http.end();
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode == 404) return FEED_UNSUPPORTED;
    return httpCode < 0 ? NETWORK_ERROR : SERVER_ERROR;
  }
  const String body = http.getString();
  http.end();
  return parseChangesPage(body.c_str(), body.length(), page, callback, context);
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (!isSuccessfulHttpCode(httpCode)) {
    http.end();
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode == 404) return FEED_UNSUPPORTED;
    return SERVER_ERROR;
  }
  const std::string& body = http.getString();
  const Error result = parseChangesPage(body.c_str(), body.size(), page, callback, context);
  http.end();
  return result;
#endif
}

bool KOReaderSyncClient::readerId(char* out, const size_t capacity) {
#ifdef SIMULATOR
  // Deliberately separate from hardware identities; never overwrite a reader.
  const int n = snprintf(out, capacity, "crossink-simulator");
#else
  uint8_t mac[6]{};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
  const int n =
      snprintf(out, capacity, "crossink-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
  return n > 0 && static_cast<size_t>(n) < capacity;
}

std::string KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return tr(STR_NO_CREDENTIALS_MSG);
    case NETWORK_ERROR:
      return networkErrorMessage();
    case AUTH_FAILED:
      return tr(STR_KOREADER_SYNC_AUTH_REJECTED);
    case SERVER_ERROR:
      if (lastHttpCode == 404) return tr(STR_KOREADER_SYNC_HTTP_404);
      if (lastHttpCode > 0) return formatHttpStatusMessage(lastHttpCode);
      return tr(STR_KOREADER_SYNC_SERVER_ERROR);
    case JSON_ERROR:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case NOT_FOUND:
      return tr(STR_NO_REMOTE_MSG);
    case INVALID_AUTH_RESPONSE:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case LOW_MEMORY:
      return tr(STR_KOREADER_SYNC_LOW_MEMORY);
    case FEED_UNSUPPORTED:
    case BATCH_UNSUPPORTED:
      return tr(STR_KOREADER_SYNC_HTTP_404);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}

void KOReaderMetadata::loadExtended(Epub& epub) {
  if (!KOREADER_STORE.supportsExtendedMetadata()) return;
  Epub::SyncMetadata metadata;
  if (!epub.loadSyncMetadata(metadata)) {
    LOG_ERR("KOSync", "Cannot read extended metadata");
    return;
  }
  isbn = std::move(metadata.isbn);
  asin = std::move(metadata.asin);
  series = std::move(metadata.series);
  char* end = nullptr;
  const float index = std::strtof(metadata.seriesIndex.c_str(), &end);
  if (end != metadata.seriesIndex.c_str() && *end == '\0' && std::isfinite(index)) seriesIndex = index;
}
