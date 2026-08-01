#include "BookOrbitSyncClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HTTPClient.h>
#include <I18n.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#endif

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#include "BookOrbitCredentialStore.h"

// This file intentionally mirrors lib/KOReaderSync/KOReaderSyncClient.cpp rather than
// sharing its HTTP plumbing. BookOrbit was added as a separate, non-breaking sync
// provider (see AGENTS.md / SCOPE.md discussion): keeping the two clients independent
// means BookOrbit support cannot regress the existing generic KOReader sync path.

int BookOrbitSyncClient::lastHttpCode = 0;
int BookOrbitSyncClient::lastTransportError = 0;

namespace {
constexpr char DEVICE_ID[] = "crossink-device";

std::string formatHttpStatusMessage(int httpCode) {
  char buffer[96];
  snprintf(buffer, sizeof(buffer), tr(STR_KOREADER_SYNC_HTTP_STATUS_FORMAT), httpCode);
  return std::string(buffer);
}

std::string networkErrorMessage() {
#ifdef SIMULATOR
  switch (BookOrbitSyncClient::lastTransportError) {
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
  switch (BookOrbitSyncClient::lastTransportError) {
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
      return tr(STR_KOREADER_SYNC_NETWORK_REFUSED);
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_INCOMPLETE_DATA:
      return tr(STR_KOREADER_SYNC_NETWORK_TIMEOUT);
    case ESP_ERR_HTTP_INVALID_TRANSPORT:
      return tr(STR_KOREADER_SYNC_NETWORK_TLS);
    default:
      return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
  }
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

  LOG_ERR("BookOrbit", "%s JSON parse failed: %s (%s, preview=\"%s\")", context, error.c_str(), classifyJsonBody(body),
          preview);
}

BookOrbitSyncClient::Error validateAuthResponse(const char* body) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body ? body : "");
  if (error) {
    logJsonParseFailure("Auth", error, body);
    return BookOrbitSyncClient::JSON_ERROR;
  }

  if (!doc.is<JsonObject>()) {
    LOG_ERR("BookOrbit", "Auth response was not a JSON object");
    return BookOrbitSyncClient::INVALID_AUTH_RESPONSE;
  }

  const char* authorized = doc["authorized"] | nullptr;
  if (authorized && std::strcmp(authorized, "OK") != 0) {
    LOG_ERR("BookOrbit", "Auth response explicitly denied authorization");
    return BookOrbitSyncClient::INVALID_AUTH_RESPONSE;
  }

  return BookOrbitSyncClient::OK;
}

// See the identical comment in KOReaderSyncClient.cpp: TLS handshakes on the ESP32-C3
// collectively consume tens of KB of heap, so we refuse to even attempt one below this
// floor rather than risk an aggregate-exhaustion allocation failure mid-handshake.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

#ifdef SIMULATOR
void addAuthHeaders(HTTPClient& http) {
  // BookOrbit's own KOReader plugin sends plain application/json (not the kosync
  // vendor type application/vnd.koreader.v1+json), so mirror that here.
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", BOOKORBIT_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", BOOKORBIT_STORE.getMd5Password().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
#else
// Small TLS buffers to fit in ESP32-C3's limited heap (~46KB free after WiFi).
// Sync payloads are tiny JSON (<1KB), so 2KB buffers are sufficient.
// Default 16KB buffers cause OOM during TLS handshake.
constexpr int HTTP_BUF_SIZE = 2048;

void logHeapStats(const char* phase, const char* url = nullptr) {
  LOG_DBG("BookOrbit", "%s%s%s heap: free=%u min=%u max_alloc=%u", phase, url ? " " : "", url ? url : "",
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

// Response buffer for reading HTTP body
struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    char* newData = (char*)realloc(data, size);
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

// HTTP event handler to collect response body
esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("BookOrbit", "Response buffer allocation failed (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = 15000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  // BookOrbit's kosync-compatible auth headers. Accept is plain application/json to
  // mirror BookOrbit's own KOReader plugin (not the kosync vendor type).
  if (esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-user", BOOKORBIT_STORE.getUsername().c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-key", BOOKORBIT_STORE.getMd5Password().c_str()) != ESP_OK) {
    LOG_ERR("BookOrbit", "Failed to set auth headers");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  return client;
}
#endif
}  // namespace

BookOrbitSyncClient::Error BookOrbitSyncClient::authenticate() {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/users/auth";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BookOrbit", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

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

  LOG_DBG("BookOrbit", "Auth response: %d", httpCode);

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();
    return validateAuthResponse(responseBody.c_str());
  }

  http.end();

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before auth client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  logHeapStats("Before auth perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After auth perform");
  esp_http_client_cleanup(client);

  LOG_DBG("BookOrbit", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return validateAuthResponse(buf.data);
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

BookOrbitSyncClient::Error BookOrbitSyncClient::getProgress(const std::string& documentHash,
                                                            KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BookOrbit", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

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

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      logJsonParseFailure("Get progress", error, responseBody.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("BookOrbit", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  LOG_DBG("BookOrbit", "Get progress response: %d", httpCode);

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before get client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  logHeapStats("Before get perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After get perform");
  esp_http_client_cleanup(client);

  LOG_DBG("BookOrbit", "Get progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);

    if (error) {
      logJsonParseFailure("Get progress", error, buf.data);
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("BookOrbit", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
#endif
}

BookOrbitSyncClient::Error BookOrbitSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/syncs/progress";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BookOrbit", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  // Build JSON body. BookOrbit additionally understands "timestamp" (unlike a plain
  // koreader-sync server) to break ties when two devices report the same percentage;
  // callers populate progress.timestamp from wall-clock time after an NTP sync.
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = progress.device;
  doc["device_id"] = DEVICE_ID;
  if (progress.timestamp > 0) {
    doc["timestamp"] = progress.timestamp;
  }

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("BookOrbit", "Request body: %s", body.c_str());

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
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();

  LOG_DBG("BookOrbit", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before put client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("BookOrbit", "Failed to set request body");
    lastTransportError = ESP_ERR_INVALID_STATE;
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  LOG_DBG("BookOrbit", "PUT body bytes=%u", static_cast<unsigned>(body.length()));
  logHeapStats("Before put perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After put perform");
  esp_http_client_cleanup(client);

  LOG_DBG("BookOrbit", "Update progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

BookOrbitSyncClient::Error BookOrbitSyncClient::uploadPageStats(const std::string& documentHash,
                                                                const std::string& deviceModel,
                                                                const BookOrbitStatEvent* events, size_t count) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (!events || count == 0) {
    return OK;
  }

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/page-stats";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Uploading %u stat events: %s (heap: %u)", (unsigned)count, url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BookOrbit", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  // Body shape mirrors BookOrbit's own KOReader plugin: device fields at the top
  // level plus books[].hash and books[].events[].{page,startTime,durationSeconds,totalPages}.
  // pluginVersion: the server caps this field at 20 characters and rejects the whole
  // upload with HTTP 400 beyond that (verified against a live server by the samfoy
  // fork). Never build it from CROSSINK_VERSION — variant/branch builds overflow
  // (e.g. "crossink-1.4.0-xlarge" is 21 chars). Bump the numeric suffix when the
  // payload shape changes.
  // The JsonDocument is scoped so its node pool is freed before the TLS session
  // starts: only the serialized body stays alive through the handshake.
  std::string body;
  {
    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";
    char deviceTime[20];
    const time_t now = time(nullptr);
    struct tm nowUtc = {};
    gmtime_r(&now, &nowUtc);
    strftime(deviceTime, sizeof(deviceTime), "%Y-%m-%d %H:%M:%S", &nowUtc);
    doc["deviceTime"] = deviceTime;

    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    JsonArray jsonEvents = book["events"].to<JsonArray>();
    for (size_t i = 0; i < count; i++) {
      JsonObject event = jsonEvents.add<JsonObject>();
      event["page"] = events[i].page;
      event["startTime"] = events[i].startTime;
      event["durationSeconds"] = events[i].durationSeconds;
      event["totalPages"] = events[i].totalPages;
    }
    serializeJson(doc, body);
  }

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

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();

  LOG_DBG("BookOrbit", "Upload stats response: %d", httpCode);

  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before stats client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("BookOrbit", "Failed to set request body");
    lastTransportError = ESP_ERR_INVALID_STATE;
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  LOG_DBG("BookOrbit", "POST body bytes=%u", static_cast<unsigned>(body.length()));
  logHeapStats("Before stats perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After stats perform");
  esp_http_client_cleanup(client);

  LOG_DBG("BookOrbit", "Upload stats response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

std::string BookOrbitSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return tr(STR_BOOKORBIT_SETUP_HINT);
    case NETWORK_ERROR:
      return networkErrorMessage();
    case AUTH_FAILED:
      return tr(STR_KOREADER_SYNC_AUTH_REJECTED);
    case SERVER_ERROR:
      if (lastHttpCode == 404) return tr(STR_BOOKORBIT_SYNC_HTTP_404);
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
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
