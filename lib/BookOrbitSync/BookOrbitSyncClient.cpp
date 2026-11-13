#include "BookOrbitSyncClient.h"

#include <AppVersion.h>
#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HTTPClient.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_mac.h>
#ifdef SIMULATOR
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>

#include "BookOrbitCredentialStore.h"
#include "StreamingJsonParser.h"

// This file intentionally mirrors lib/KOReaderSync/KOReaderSyncClient.cpp rather than
// sharing its HTTP plumbing. BookOrbit was added as a separate, non-breaking sync
// provider (see AGENTS.md / SCOPE.md discussion): keeping the two clients independent
// means BookOrbit support cannot regress the existing generic KOReader sync path.

int BookOrbitSyncClient::lastHttpCode = 0;
int BookOrbitSyncClient::lastTransportError = 0;

const char* BookOrbitSyncClient::deviceId() {
  static const std::array<char, 24> id = [] {
    std::array<char, 24> value{};
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) != 0) {
      LOG_ERR("BookOrbit", "Could not read factory MAC; falling back to shared device id");
      snprintf(value.data(), value.size(), "crossink-device");
      return value;
    }
    snprintf(value.data(), value.size(), "crossink-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return value;
  }();
  return id.data();
}

namespace {
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

// The wire value of a stated sidecar state, nullptr when the field is left out.
const char* sidecarStateName(const BookOrbitSidecarState state) {
  switch (state) {
    case BookOrbitSidecarState::Known:
      return "known";
    case BookOrbitSidecarState::New:
      return "new";
    case BookOrbitSidecarState::Unstated:
      break;
  }
  return nullptr;
}

// What the server said about the sidecarState capability this boot (see acceptsSidecarState).
enum class CapabilityAnswer : uint8_t { Unknown, Yes, No };
CapabilityAnswer s_sidecarStateAnswer = CapabilityAnswer::Unknown;

// Why the last request's reply was not read to its end, if it was not (see
// sendBookOrbitRequest). Never set on the simulator, whose HTTP client buffers on a host heap.
enum class BodyRefusal : uint8_t { None, TooLarge, LowMemory };
BodyRefusal s_lastBodyRefusal = BodyRefusal::None;

// What a request that returned no usable status means to its caller.
BookOrbitSyncClient::Error transportFailure() {
  switch (s_lastBodyRefusal) {
    case BodyRefusal::LowMemory:
      return BookOrbitSyncClient::LOW_MEMORY;
    case BodyRefusal::TooLarge:
      return BookOrbitSyncClient::INVALID_AUTH_RESPONSE;
    case BodyRefusal::None:
      break;
  }
  return BookOrbitSyncClient::NETWORK_ERROR;
}

// See the identical comment in KOReaderSyncClient.cpp: TLS handshakes on the ESP32-C3
// collectively consume tens of KB of heap, so we refuse to even attempt one below this
// floor rather than risk an aggregate-exhaustion allocation failure mid-handshake.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// A request that reuses the Session's established connection pays no handshake, and the
// full floor would double-count the memory that live TLS session already holds -- it
// refused affordable uploads whenever a request followed another in the same session
// (stats after the progress fetch, bookmarks after highlights: measured 54804 free
// against the 55000 floor). Such a request only needs its JSON body and framing.
//
// This floor bounds the connection's cost, NOT the body's: a caller that builds a body
// too large for what is left still has to fail gracefully, which is why bodies are
// measured and allocated without throwing (see JsonBody). Callers keep their batches
// small for the same reason -- 25 stat events, 8 annotations.
constexpr uint32_t MIN_HEAP_FOR_KEPT_SESSION = 20000;

// True from the first completed request on the shared session until the Session ends.
// Never set on the simulator, which keeps the full floor. Blind spot: a server dropping
// the keep-alive mid-session makes the next request re-handshake under the small floor;
// wolfSSL fails an unaffordable handshake cleanly, so that degrades to NETWORK_ERROR
// and a retry on the next sync rather than a crash.
bool s_sessionHandshakePaid = false;

uint32_t requiredHeapFloor() { return s_sessionHandshakePaid ? MIN_HEAP_FOR_KEPT_SESSION : MIN_HEAP_FOR_TLS; }

/**
 * One exactly-sized request body, allocated without throwing.
 *
 * Serializing into a std::string costs more than the body: its growth doubles, so
 * the last reallocation holds both halves at once, and with exceptions disabled a
 * failed allocation inside it calls abort() rather than returning -- the firmware
 * dies where it could have reported a failure. That is the measured crash on a
 * 100-event stats batch: ~8KB of body plus the doubling, admitted by the
 * kept-session floor and then unaffordable.
 *
 * Measuring first costs one pass over the document and turns the same situation
 * into a LOW_MEMORY return. Build inside the JsonDocument's scope so the node pool
 * is freed before the request runs; only this buffer stays alive through it.
 */
class JsonBody {
 public:
  // `room`: bytes set aside past the document, for insertBeforeEnd().
  bool build(const JsonDocument& doc, const size_t room = 0) {
    const size_t length = measureJson(doc);
    data_ = makeUniqueNoThrow<char[]>(length + room + 1);
    if (!data_) {
      LOG_ERR("BookOrbit", "Out of memory for a %u byte request body (heap: %u free, %u max alloc)",
              (unsigned)(length + room + 1), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return false;
    }
    length_ = serializeJson(doc, data_.get(), length + 1);
    capacity_ = length_ + room;
    return true;
  }

  // Opens a gap of `len` bytes `fromEnd` bytes before the end of the body, in the room build()
  // set aside, for the caller to fill. nullptr when it does not fit.
  char* insertBeforeEnd(const size_t fromEnd, const size_t len) {
    if (!data_ || fromEnd > length_ || length_ + len > capacity_) return nullptr;
    char* gap = data_.get() + length_ - fromEnd;
    memmove(gap + len, gap, fromEnd + 1);  // the tail and its terminator
    length_ += len;
    return gap;
  }

  const char* c_str() const { return data_ ? data_.get() : ""; }
  const uint8_t* bytes() const { return reinterpret_cast<const uint8_t*>(c_str()); }
  size_t length() const { return length_; }

  // Frees the body once it has been sent: the response still has to be parsed, and
  // an exchange's request and response should not be held at once on this hardware.
  void release() {
    data_.reset();
    length_ = 0;
    capacity_ = 0;
  }

 private:
  std::unique_ptr<char[]> data_;
  size_t length_ = 0;
  size_t capacity_ = 0;
};

// An annotation exchange's key set, written straight into its body rather than built as JSON
// nodes: those cost ~100 bytes a key on top of the ~70 each serializes to, which held a key set
// to 64 keys beside a TLS session, and a book with more never told the server what it deleted.
// It goes last in the book object, so it is inserted before the body's closing "}]}" (the
// book, the books array, the document). Keys are hex digests and "YYYY-MM-DD HH:MM:SS"
// datetimes, so nothing in them needs escaping.
constexpr char KEYS_OPEN[] = ",\"keys\":[";
constexpr char KEY_OPEN[] = "{\"k\":\"";
constexpr char KEY_MIDDLE[] = "\",\"dt\":\"";
constexpr char KEY_CLOSE[] = "\"}";
constexpr char BODY_END[] = "}]}";

size_t keySetJsonLength(const BookOrbitAnnotationKeys& keys) {
  size_t length = sizeof(KEYS_OPEN) - 1 + 1;  // and the closing bracket
  for (size_t i = 0; keys.rows && i < keys.count; i++) {
    length += (i > 0 ? 1 : 0) + sizeof(KEY_OPEN) - 1 + strlen(keys.rows[i].k) + sizeof(KEY_MIDDLE) - 1 +
              strlen(keys.rows[i].dt) + sizeof(KEY_CLOSE) - 1;
  }
  return length;
}

bool writeKeySet(JsonBody& body, const BookOrbitAnnotationKeys& keys, const size_t length) {
  constexpr size_t endLength = sizeof(BODY_END) - 1;
  if (body.length() < endLength || strcmp(body.c_str() + body.length() - endLength, BODY_END) != 0) {
    LOG_ERR("BookOrbit", "Exchange body does not end with its book object; key set not added");
    return false;
  }
  char* out = body.insertBeforeEnd(endLength, length);
  if (!out) return false;
  const auto put = [&out](const char* text, const size_t len) {
    memcpy(out, text, len);
    out += len;
  };
  put(KEYS_OPEN, sizeof(KEYS_OPEN) - 1);
  for (size_t i = 0; keys.rows && i < keys.count; i++) {
    if (i > 0) put(",", 1);
    put(KEY_OPEN, sizeof(KEY_OPEN) - 1);
    put(keys.rows[i].k, strlen(keys.rows[i].k));
    put(KEY_MIDDLE, sizeof(KEY_MIDDLE) - 1);
    put(keys.rows[i].dt, strlen(keys.rows[i].dt));
    put(KEY_CLOSE, sizeof(KEY_CLOSE) - 1);
  }
  put("]", 1);
  return true;
}

#ifdef SIMULATOR
void addAuthHeaders(HTTPClient& http) {
  // BookOrbit's own KOReader plugin sends plain application/json (not the kosync
  // vendor type application/vnd.koreader.v1+json), so mirror that here.
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", BOOKORBIT_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", BOOKORBIT_STORE.getMd5Password().c_str());
  if (BOOKORBIT_STORE.sendsCfAccessToken()) {
    http.addHeader("CF-Access-Client-Id", BOOKORBIT_STORE.getCfClientId().c_str());
    http.addHeader("CF-Access-Client-Secret", BOOKORBIT_STORE.getCfClientSecret().c_str());
  }
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

// Every BookOrbit request goes over wolfSSL rather than esp_http_client/mbedTLS. mbedTLS
// cannot parse a modern certificate chain within the heap left after WiFi on this chip: it
// fails the handshake with MBEDTLS_ERR_X509_FATAL_ERROR, which is what broke sync and the
// catalog against servers on Let's Encrypt's four-certificate path. The catalog moved first
// (see BookOrbitCatalogClient); this brings the sync endpoints across too.
//
// Returns the HTTP status, or a negative value when the transport itself failed. The
// response body, if any, lands in outBody.
//
// The body is read through a bounded sink rather than the client's own buffer. That buffer
// is a std::string: its growth doubles, the caller then had to copy it, and with exceptions
// disabled an allocation it cannot make calls abort() -- with the TLS session still open,
// when the heap is at its tightest. CrossInk v1.6.1 capped the kosync auth reply for this
// reason; here every reply takes the one path. It lands straight in outBody, grown in exact
// steps the heap is asked about first, so a reply this device cannot hold ends the request
// (transportFailure() then says why) instead of the firmware.
constexpr int BODY_REFUSED = -2;
constexpr size_t RESPONSE_GROWTH_STEP = 1024;
// Left as one contiguous block after a growth step: room for a TLS record in flight. Kept
// that small on purpose, so that a heap a short reply fitted in before still takes it.
constexpr uint32_t RESPONSE_HEAP_MARGIN = HTTP_BUF_SIZE;
constexpr size_t NO_BODY_LIMIT = SIZE_MAX;
// The auth reply is a small JSON object. Anything larger is an error page or the wrong server.
constexpr size_t MAX_AUTH_RESPONSE_BYTES = 4096;
// The version reply is a few hundred bytes of JSON, so the same bound applies.
constexpr size_t MAX_VERSION_RESPONSE_BYTES = 4096;
// Non-null while a BookOrbitSyncClient::Session is alive; requests then share its socket.
std::unique_ptr<freeink::SecureHttpClient> s_session;

void configureClient(freeink::SecureHttpClient& http, const bool reuse) {
  http.setUserAgent(AppVersion::userAgent());
  http.setTimeout(15000);
  http.setReuse(reuse);
  // Unverified, like every other network path in this firmware: wolfSSL is the only
  // transport whose handshake fits this chip's heap, and it has no access to ESP-IDF's root
  // bundle (that bundle verifies through mbedTLS's own primitives). Credentials are still
  // encrypted in transit, but the server's identity is not authenticated.
  http.setInsecure();
}

// Receives a reply's body as it arrives, with the length the server announced (0 when it did not).
// Returning false stops the reading, which closes the connection.
using ReplySink = std::function<bool(const uint8_t* data, size_t len, size_t announced)>;

int sendBookOrbitRequest(const char* method, const std::string& url, const JsonBody* payload, const ReplySink& onBody) {
  s_lastBodyRefusal = BodyRefusal::None;
  const bool pooled = s_session != nullptr;
  freeink::SecureHttpClient oneShot;
  freeink::SecureHttpClient& http = pooled ? *s_session : oneShot;
  if (!pooled) {
    configureClient(http, false);
  }
  logHeapStats("Before request", url.c_str());
  if (!http.begin(url)) {
    LOG_ERR("BookOrbit", "Failed to open connection: %s", url.c_str());
    return -1;
  }
  // BookOrbit's kosync-compatible auth headers. Accept is plain application/json to mirror
  // BookOrbit's own KOReader plugin (not the kosync vendor type).
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", BOOKORBIT_STORE.getUsername());
  http.addHeader("x-auth-key", BOOKORBIT_STORE.getMd5Password());
  if (BOOKORBIT_STORE.sendsCfAccessToken()) {
    http.addHeader("CF-Access-Client-Id", BOOKORBIT_STORE.getCfClientId());
    http.addHeader("CF-Access-Client-Secret", BOOKORBIT_STORE.getCfClientSecret());
  }
  if (payload != nullptr) {
    http.addHeader("Content-Type", "application/json");
  }
  size_t received = 0;
  int code = http.sendRequest(method, payload != nullptr ? payload->bytes() : nullptr,
                              payload != nullptr ? payload->length() : 0, [&](const uint8_t* data, const size_t len) {
                                received += len;
                                return onBody(data, len, http.getContentLength());
                              });
  const bool bodyRefused = s_lastBodyRefusal != BodyRefusal::None;
  if (bodyRefused) {
    LOG_ERR("BookOrbit", "Reply not read (HTTP %d): %s (%u bytes in, announced %u, heap: %u free, %u max alloc)", code,
            s_lastBodyRefusal == BodyRefusal::TooLarge ? "larger than this request allows" : "not enough memory",
            static_cast<unsigned>(received), static_cast<unsigned>(http.getContentLength()),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    code = BODY_REFUSED;
  }
  // An HTTP status, even an error one, proves the session's handshake is up and paid
  // for; a transport failure may have closed the socket, and stopping a reply short
  // always does, so the next request assumes it pays a new one.
  if (pooled) {
    s_sessionHandshakePaid = code > 0;
  }
  // A pooled client keeps its socket for the next request; the Session closes it.
  if (!pooled) {
    http.end();
  }
  logHeapStats("After request");
  return code;
}

// A reply read whole into outBody, for the requests whose replies stay small.
int sendBookOrbitRequest(const char* method, const std::string& url, const JsonBody* payload, std::string& outBody,
                         const size_t maxBodyBytes = NO_BODY_LIMIT) {
  outBody.clear();
  const int code = sendBookOrbitRequest(
      method, url, payload, [&outBody, maxBodyBytes](const uint8_t* data, const size_t len, const size_t announced) {
        const size_t needed = outBody.size() + len;
        if (needed > maxBodyBytes) {
          s_lastBodyRefusal = BodyRefusal::TooLarge;
          return false;
        }
        if (needed > outBody.capacity()) {
          // The whole body in one allocation when the server announced its length, a step otherwise.
          const size_t expected = std::min(announced, maxBodyBytes);
          const size_t target = expected >= needed
                                    ? expected
                                    : (needed + RESPONSE_GROWTH_STEP - 1) / RESPONSE_GROWTH_STEP * RESPONSE_GROWTH_STEP;
          if (ESP.getMaxAllocHeap() < target + 1 + RESPONSE_HEAP_MARGIN) {
            s_lastBodyRefusal = BodyRefusal::LowMemory;
            return false;
          }
          outBody.reserve(target);
        }
        outBody.append(reinterpret_cast<const char*>(data), len);
        return true;
      });
  if (code == BODY_REFUSED) {
    outBody.clear();
    outBody.shrink_to_fit();
  }
  return code;
}

#endif
}  // namespace

#ifdef SIMULATOR
BookOrbitSyncClient::Session::Session() {}
BookOrbitSyncClient::Session::~Session() {}
#else
BookOrbitSyncClient::Session::Session() {
  s_sessionHandshakePaid = false;
  s_session = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!s_session) {
    LOG_ERR("BookOrbit", "Session allocation failed; requests will open their own connections");
    return;
  }
  configureClient(*s_session, true);
  LOG_DBG("BookOrbit", "Sync session open: requests share one TLS connection");
}

BookOrbitSyncClient::Session::~Session() {
  s_sessionHandshakePaid = false;
  if (s_session) {
    s_session->end();
    s_session.reset();
    LOG_DBG("BookOrbit", "Sync session closed");
  }
}
#endif

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
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
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
  if (httpCode < 0) return transportFailure();
  return SERVER_ERROR;
#else
  std::string body;
  const int httpCode = sendBookOrbitRequest("GET", url, nullptr, body, MAX_AUTH_RESPONSE_BYTES);
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_DBG("BookOrbit", "Auth response: %d", httpCode);

  if (httpCode < 0) return transportFailure();
  if (httpCode == 200) return validateAuthResponse(body.c_str());
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
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
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
  if (httpCode < 0) return transportFailure();
  return SERVER_ERROR;
#else
  std::string body;
  const int httpCode = sendBookOrbitRequest("GET", url, nullptr, body);
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_DBG("BookOrbit", "Get progress response: %d", httpCode);

  if (httpCode < 0) return transportFailure();

  if (httpCode == 200 && !body.empty()) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);

    if (error) {
      logJsonParseFailure("Get progress", error, body.c_str());
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
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
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
  doc["device_id"] = deviceId();
  if (progress.timestamp > 0) {
    doc["timestamp"] = progress.timestamp;
  }

  JsonBody body;
  if (!body.build(doc)) return LOW_MEMORY;

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
  if (httpCode < 0) return transportFailure();
  return SERVER_ERROR;
#else
  LOG_DBG("BookOrbit", "PUT body bytes=%u", static_cast<unsigned>(body.length()));
  std::string response;
  const int httpCode = sendBookOrbitRequest("PUT", url, &body, response);
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_DBG("BookOrbit", "Update progress response: %d", httpCode);

  if (httpCode < 0) return transportFailure();
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
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
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
  JsonBody body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId();
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";
    char deviceTime[20] = {};
    bookOrbitFormatDatetime(static_cast<uint32_t>(time(nullptr)), deviceTime);
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
    if (!body.build(doc)) return LOW_MEMORY;
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
  if (httpCode < 0) return transportFailure();
  return SERVER_ERROR;
#else
  LOG_DBG("BookOrbit", "POST body bytes=%u", static_cast<unsigned>(body.length()));
  std::string response;
  const int httpCode = sendBookOrbitRequest("POST", url, &body, response);
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_DBG("BookOrbit", "Upload stats response: %d", httpCode);

  if (httpCode < 0) return transportFailure();
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

BookOrbitSyncClient::Error BookOrbitSyncClient::completeSweep(const std::string& deviceModel,
                                                              const uint32_t booksMatched,
                                                              const uint32_t pageStatsUploaded,
                                                              const uint32_t annotationsUpserted) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/sweeps";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Recording sweep: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
    return LOW_MEMORY;
  }

  // See uploadPageStats for why pluginVersion is a literal and the JsonDocument is scoped.
  JsonBody body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId();
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";
    char deviceTime[20] = {};
    if (bookOrbitFormatDatetime(static_cast<uint32_t>(time(nullptr)), deviceTime)) {
      // Optional server-side, but an empty string fails its format validation with HTTP 400
      // and takes the whole sweep down with it: better omitted when the clock is implausible.
      doc["deviceTime"] = deviceTime;
    }
    doc["booksMatched"] = booksMatched;
    doc["pageStatsUploaded"] = pageStatsUploaded;
    doc["annotationsUpserted"] = annotationsUpserted;
    if (!body.build(doc)) return LOW_MEMORY;
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

  LOG_DBG("BookOrbit", "Sweep response: %d", httpCode);

  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return transportFailure();
  return SERVER_ERROR;
#else
  LOG_DBG("BookOrbit", "POST body bytes=%u", static_cast<unsigned>(body.length()));
  std::string response;
  const int httpCode = sendBookOrbitRequest("POST", url, &body, response);
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_DBG("BookOrbit", "Sweep response: %d", httpCode);

  if (httpCode < 0) return transportFailure();
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

bool BookOrbitSyncClient::acceptsSidecarState() {
  if (s_sidecarStateAnswer != CapabilityAnswer::Unknown) return s_sidecarStateAnswer == CapabilityAnswer::Yes;
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return false;

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/version";
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
    return false;
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
  std::string body = httpCode == 200 ? http.getString().c_str() : "";
  http.end();
#else
  std::string body;
  const int httpCode = sendBookOrbitRequest("GET", url, nullptr, body, MAX_VERSION_RESPONSE_BYTES);
#endif
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;

  if (httpCode == 404) {
    LOG_INF("BookOrbit", "Server lists no capabilities; sidecar state left unstated until reboot");
    s_sidecarStateAnswer = CapabilityAnswer::No;
    return false;
  }
  if (httpCode != 200) {
    LOG_ERR("BookOrbit", "Capability check failed (http=%d); sidecar state left unstated this sync", httpCode);
    return false;
  }

  JsonDocument filter;
  filter["capabilities"] = true;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (error) {
    logJsonParseFailure("Capabilities", error, body.c_str());
    return false;
  }
  bool supported = false;
  for (JsonVariantConst name : doc["capabilities"].as<JsonArrayConst>()) {
    if (std::strcmp(name | "", "sidecarState") == 0) {
      supported = true;
      break;
    }
  }
  s_sidecarStateAnswer = supported ? CapabilityAnswer::Yes : CapabilityAnswer::No;
  LOG_INF("BookOrbit", "Server %s sidecar state", supported ? "accepts" : "does not accept");
  return supported;
}

namespace {
/**
 * An annotation exchange reply, read as it arrives.
 *
 * Every reply carries a page of the server's own changes (BookOrbit's PUSH_DOWN_PAGE, 100
 * entries) with their notes, colors and full texts, while a sync takes BOOKORBIT_ANNOTATION_BATCH
 * of them at a time. Read whole, a page of web highlights is tens of KB, which this chip does not
 * have beside a TLS session: the reply buffer's allocation aborted on an X4 with 60 waiting (its
 * largest-free-block check raced the Wi-Fi tasks' own allocations), and refusing such a reply
 * cleanly would still leave the book unable to sync while the server holds that page. Streamed,
 * only the entry under way and the ones kept are ever in memory.
 */
class ExchangeReplyReader {
 public:
  ExchangeReplyReader(const std::string& documentHash, std::vector<BookOrbitIncomingAnnotation>* incoming,
                      const std::vector<uint32_t>* skipServerIds)
      : documentHash(documentHash),
        incoming(incoming),
        skipServerIds(skipServerIds),
        parser(JsonCallbacks{this, onKey, onString, onNumber, onBool, nullptr, onObjectStart, onObjectEnd, onArrayStart,
                             onArrayEnd, onStringPart}) {}

  void feed(const uint8_t* data, const size_t len) { parser.feed(reinterpret_cast<const char*>(data), len); }

  // Parsed to its end, and it either answered for a book or said it does not know this one.
  bool complete() const { return !parser.hasError() && depth == 0 && (sawResult || unmatched); }

  bool unmatched = false;
  bool more = false;
  uint32_t skippedNoPosition = 0;
  size_t offered = 0;          // valid changes in results[0], skipped ones aside
  size_t collected = 0;        // of which appended to `incoming`
  uint32_t deviceDeleted = 0;  // highlights deleted here that the key set made the server delete

 private:
  enum class Key : uint8_t {
    Other,
    Unmatched,
    Results,
    ToApply,
    Add,
    Delete,
    More,
    Skipped,
    Applied,
    DeviceDeleted,
    ServerId,
    Version,
    Datetime,
    Pos0,
    EntryKey,
    Text,
    Chapter
  };
  // {"unmatched": [hash...], "results": [{"more", "skippedNoPosition", "applied": {"deviceDeleted"},
  // "toApply": {"add": [{entry}], "delete": [{entry}]}}]}: the depth of each container this reader
  // looks into.
  static constexpr uint8_t LIST = 2;
  static constexpr uint8_t RESULT = 3;
  static constexpr uint8_t TO_APPLY = 4;
  static constexpr uint8_t CHANGES = 5;
  static constexpr uint8_t ENTRY = 6;
  static constexpr uint8_t TRACKED = ENTRY + 1;

  static ExchangeReplyReader& self(void* ctx) { return *static_cast<ExchangeReplyReader*>(ctx); }

  static Key classify(const char* key) {
    static constexpr struct {
      const char* name;
      Key key;
    } KEYS[] = {{"unmatched", Key::Unmatched},
                {"results", Key::Results},
                {"toApply", Key::ToApply},
                {"add", Key::Add},
                {"delete", Key::Delete},
                {"more", Key::More},
                {"skippedNoPosition", Key::Skipped},
                {"applied", Key::Applied},
                {"deviceDeleted", Key::DeviceDeleted},
                {"serverId", Key::ServerId},
                {"version", Key::Version},
                {"datetime", Key::Datetime},
                {"pos0", Key::Pos0},
                {"key", Key::EntryKey},
                {"text", Key::Text},
                {"chapter", Key::Chapter}};
    for (const auto& entry : KEYS) {
      if (strcmp(key, entry.name) == 0) return entry.key;
    }
    return Key::Other;
  }

  // The key in force in the container at `depth`: an object's last key, an array's own key.
  Key keyAt(const uint8_t d) const { return d < TRACKED ? (isArray[d] ? containerKey[d] : lastKey[d]) : Key::Other; }
  bool inEntry() const { return entryOpen && depth == ENTRY; }

  void open(const bool array) {
    const Key under = depth < TRACKED ? keyAt(depth) : Key::Other;
    depth++;
    if (depth >= TRACKED) return;
    isArray[depth] = array;
    containerKey[depth] = under;
    lastKey[depth] = Key::Other;
    if (array) return;
    if (depth == RESULT && containerKey[LIST] == Key::Results && isArray[LIST]) {
      inFirstResult = resultCount++ == 0;
      sawResult = sawResult || inFirstResult;
    } else if (depth == ENTRY && inFirstResult && containerKey[TO_APPLY] == Key::ToApply && isArray[CHANGES] &&
               (containerKey[CHANGES] == Key::Add || containerKey[CHANGES] == Key::Delete)) {
      startEntry(containerKey[CHANGES] == Key::Delete);
    }
  }

  void close() {
    if (depth == ENTRY && entryOpen) finishEntry();
    if (depth == RESULT) inFirstResult = false;
    if (depth > 0) depth--;
  }

  void startEntry(const bool deleted) {
    entryOpen = true;
    entry.serverId = 0;
    entry.version = 0;
    entry.datetime[0] = '\0';
    entry.key.clear();
    entry.pos0.clear();
    entry.text.clear();
    entry.chapter.clear();
    entry.deleted = deleted;
  }

  void finishEntry() {
    entryOpen = false;
    if (!incoming) return;
    // Each kind carries what it needs and nothing more: a deletion has a key, an add has a
    // position. Demanding a position of both is what dropped every deletion on the floor.
    if (entry.serverId == 0 || (entry.deleted ? entry.key.empty() : entry.pos0.empty())) return;
    if (skipServerIds &&
        std::find(skipServerIds->begin(), skipServerIds->end(), entry.serverId) != skipServerIds->end()) {
      return;
    }
    offered++;
    // The server re-offers everything not yet acknowledged, so a later pull round repeats the
    // earlier rounds' entries; appending them again would apply and ack each one twice.
    const uint32_t serverId = entry.serverId;
    const bool alreadyCollected =
        std::any_of(incoming->begin(), incoming->end(),
                    [serverId](const BookOrbitIncomingAnnotation& seen) { return seen.serverId == serverId; });
    // Bounded on purpose: each entry carries its text, and the rest stays queued server-side
    // because nothing acknowledges it. The next pull brings it.
    if (alreadyCollected || incoming->size() >= BOOKORBIT_ANNOTATION_BATCH) return;
    if (entry.deleted) entry.version = 0;  // the deletion ack schema has no version field
    incoming->push_back(std::move(entry));
    collected++;
  }

  void appendText(const char* part, const size_t len) {
    const size_t room = BOOKORBIT_ANNOTATION_TEXT_MAX - std::min(entry.text.size(), BOOKORBIT_ANNOTATION_TEXT_MAX);
    entry.text.append(part, std::min(len, room));
  }

  static void onKey(void* ctx, const char* key, size_t) {
    ExchangeReplyReader& r = self(ctx);
    if (r.depth < TRACKED && !r.isArray[r.depth]) r.lastKey[r.depth] = classify(key);
  }

  static void onString(void* ctx, const char* value, const size_t len) {
    ExchangeReplyReader& r = self(ctx);
    if (r.depth == LIST && r.isArray[LIST] && r.containerKey[LIST] == Key::Unmatched) {
      if (r.documentHash == value) r.unmatched = true;
      return;
    }
    if (!r.inEntry() || !r.incoming) return;
    switch (r.lastKey[ENTRY]) {
      case Key::Datetime:
        snprintf(r.entry.datetime, sizeof(r.entry.datetime), "%s", value);
        break;
      case Key::Pos0:
        r.entry.pos0.assign(value, len);
        break;
      case Key::EntryKey:
        r.entry.key.assign(value, len);
        break;
      case Key::Text:
        r.appendText(value, len);
        break;
      case Key::Chapter:
        r.entry.chapter.assign(value, len);
        break;
      default:
        break;
    }
  }

  // Only a text outgrows the parser's buffer; any other value that long is dropped.
  static void onStringPart(void* ctx, const char* part, const size_t len, bool) {
    ExchangeReplyReader& r = self(ctx);
    if (r.inEntry() && r.incoming && r.lastKey[ENTRY] == Key::Text) r.appendText(part, len);
  }

  static void onNumber(void* ctx, const char* value, size_t) {
    ExchangeReplyReader& r = self(ctx);
    const auto number = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    if (r.inFirstResult && r.depth == RESULT && r.lastKey[RESULT] == Key::Skipped) {
      r.skippedNoPosition = number;
    } else if (r.inFirstResult && r.depth == TO_APPLY && r.containerKey[TO_APPLY] == Key::Applied &&
               r.lastKey[TO_APPLY] == Key::DeviceDeleted) {
      r.deviceDeleted = number;
    } else if (r.inEntry() && r.lastKey[ENTRY] == Key::ServerId) {
      r.entry.serverId = number;
    } else if (r.inEntry() && r.lastKey[ENTRY] == Key::Version) {
      r.entry.version = number;
    }
  }

  static void onBool(void* ctx, const bool value) {
    ExchangeReplyReader& r = self(ctx);
    if (r.inFirstResult && r.depth == RESULT && r.lastKey[RESULT] == Key::More) r.more = value;
  }

  static void onObjectStart(void* ctx) { self(ctx).open(false); }
  static void onArrayStart(void* ctx) { self(ctx).open(true); }
  static void onObjectEnd(void* ctx) { self(ctx).close(); }
  static void onArrayEnd(void* ctx) { self(ctx).close(); }

  const std::string& documentHash;
  std::vector<BookOrbitIncomingAnnotation>* incoming;
  const std::vector<uint32_t>* skipServerIds;
  StreamingJsonParser parser;

  uint8_t depth = 0;
  bool isArray[TRACKED] = {};
  Key containerKey[TRACKED] = {};
  Key lastKey[TRACKED] = {};
  size_t resultCount = 0;
  bool inFirstResult = false;
  bool sawResult = false;
  bool entryOpen = false;
  BookOrbitIncomingAnnotation entry;
};
}  // namespace

BookOrbitSyncClient::Error BookOrbitSyncClient::exchangeAnnotations(
    const std::string& documentHash, const std::string& deviceModel, const BookOrbitAnnotationKeys& keys,
    const BookOrbitAnnotation* changes, const size_t changeCount, bool& outUnmatched,
    std::vector<BookOrbitIncomingAnnotation>* outIncoming, bool* outMorePending,
    const std::vector<uint32_t>* skipServerIds, size_t* outOffered, uint32_t* outDeviceDeleted) {
  lastHttpCode = 0;
  if (outOffered) *outOffered = 0;
  if (outDeviceDeleted) *outDeviceDeleted = 0;
  lastTransportError = 0;
  outUnmatched = false;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }
  // No early return for an empty payload. This request is the only thing that brings the
  // server's own highlight changes down, so skipping it when the device has nothing to say is
  // exactly how a device stops hearing about deletions and web-created highlights.

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/annotations/exchange";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Exchanging %u annotations, %u keys (heap: %u)", (unsigned)changeCount, (unsigned)keys.count,
          (unsigned)freeHeap);
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
    return LOW_MEMORY;
  }

  // Scoped so the node pool is freed before the TLS session opens: only the serialized body has
  // to survive the handshake. See uploadPageStats for why pluginVersion is a literal.
  const size_t keySetLength = keySetJsonLength(keys);
  JsonBody body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId();
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";

    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    book["keysComplete"] = keys.complete;
    if (const char* sidecar = sidecarStateName(keys.sidecar)) book["sidecarState"] = sidecar;
    // "keys" is added to the serialized body below (see writeKeySet), as objects rather than
    // bare digests: the server validates {k, dt} per key and rejects the whole request with
    // HTTP 400 otherwise.

    JsonArray jsonChanges = book["changes"].to<JsonArray>();
    for (size_t i = 0; changes && i < changeCount; i++) {
      const BookOrbitAnnotation& annotation = changes[i];
      JsonObject entry = jsonChanges.add<JsonObject>();
      entry["datetime"] = annotation.datetime;
      // Required: an entry without a drawer is a position-only bookmark to the server, and
      // bookmarks travel through their own endpoint. Clippings carry no style, so the
      // plainest highlight is the honest choice.
      entry["drawer"] = "lighten";
      entry["posFormat"] = "xpointer";
      entry["pos0"] = annotation.pos0;
      entry["pos1"] = annotation.pos1;
      entry["text"] = annotation.text;
      if (!annotation.chapter.empty()) entry["chapter"] = annotation.chapter;
      if (annotation.pageno > 0) entry["pageno"] = annotation.pageno;
    }
    if (!body.build(doc, keySetLength)) return LOW_MEMORY;
  }
  if (!writeKeySet(body, keys, keySetLength)) return LOW_MEMORY;
  // Before the request, like the body: nothing of the reply's size is ever allocated.
  auto reader = makeUniqueNoThrow<ExchangeReplyReader>(documentHash, outIncoming, skipServerIds);
  if (!reader) {
    LOG_ERR("BookOrbit", "Out of memory for the exchange reply reader");
    return LOW_MEMORY;
  }
  const size_t incomingBefore = outIncoming ? outIncoming->size() : 0;

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
  if (httpCode > 0) {
    const std::string response = http.getString().c_str();
    reader->feed(reinterpret_cast<const uint8_t*>(response.data()), response.size());
  }
  http.end();
#else
  LOG_DBG("BookOrbit", "POST body bytes=%u", static_cast<unsigned>(body.length()));
  const int httpCode =
      sendBookOrbitRequest("POST", url, &body, [&reader](const uint8_t* data, const size_t len, size_t /*announced*/) {
        reader->feed(data, len);
        return true;  // read to the end, so the session's socket stays usable
      });
#endif
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  body.release();
  LOG_DBG("BookOrbit", "Annotation exchange response: %d", httpCode);

  const auto dropCollected = [&] {
    if (outIncoming) outIncoming->resize(incomingBefore);
  };
  if (httpCode < 0 || httpCode == 401 || httpCode < 200 || httpCode >= 300) dropCollected();
  if (httpCode < 0) return transportFailure();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 200 || httpCode >= 300) return SERVER_ERROR;

  if (!reader->complete()) {
    LOG_ERR("BookOrbit", "Failed to parse annotation exchange response");
    dropCollected();
    return JSON_ERROR;
  }
  if (reader->unmatched) {
    LOG_INF("BookOrbit", "Server does not know this document; annotations not exchanged");
    dropCollected();
    outUnmatched = true;
    return OK;
  }
  // `more` says the push-down page was full; skippedNoPosition counts web highlights whose
  // positions the server converted this round and can only send on the NEXT request.
  if (outMorePending) *outMorePending = reader->more || reader->skippedNoPosition > 0;
  if (outOffered) *outOffered = reader->offered;
  if (outDeviceDeleted) *outDeviceDeleted = reader->deviceDeleted;
  if (reader->collected > 0) {
    LOG_INF("BookOrbit", "Server sent %u annotation change(s), %u offered", (unsigned)reader->collected,
            (unsigned)reader->offered);
  }
  return OK;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::ackAnnotations(const std::string& documentHash,
                                                               const std::string& deviceModel,
                                                               const std::vector<BookOrbitAckEntry>& applied,
                                                               const std::vector<BookOrbitAckEntry>& deleted) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (applied.empty() && deleted.empty()) return OK;

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/annotations/exchange-ack";
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
    return LOW_MEMORY;
  }

  JsonBody body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId();
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";
    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    JsonArray appliedArray = book["applied"].to<JsonArray>();
    for (const BookOrbitAckEntry& ack : applied) {
      JsonObject entry = appliedArray.add<JsonObject>();
      entry["serverId"] = ack.serverId;
      entry["version"] = ack.version;
      entry["status"] = "applied";
    }
    JsonArray deletedArray = book["deleted"].to<JsonArray>();
    for (const BookOrbitAckEntry& ack : deleted) {
      JsonObject entry = deletedArray.add<JsonObject>();
      entry["serverId"] = ack.serverId;
      // No version here: the deletion ack schema does not define the field.
      entry["status"] = "applied";
    }
    if (!body.build(doc)) return LOW_MEMORY;
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
  http.end();
#else
  std::string response;
  const int httpCode = sendBookOrbitRequest("POST", url, &body, response);
#endif
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_INF("BookOrbit", "Annotation ack response: %d (%u applied, %u deleted)", httpCode, (unsigned)applied.size(),
          (unsigned)deleted.size());

  if (httpCode < 0) return transportFailure();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode >= 200 && httpCode < 300) return OK;
  return SERVER_ERROR;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::exchangeBookmarks(
    const std::string& documentHash, const std::string& deviceModel, const BookOrbitAnnotationKeys& keys,
    const BookOrbitBookmark* changes, const size_t changeCount, bool& outUnmatched,
    std::vector<BookOrbitIncomingBookmark>* outIncoming, bool* outMorePending) {
  lastHttpCode = 0;
  lastTransportError = 0;
  outUnmatched = false;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/bookmarks/exchange";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BookOrbit", "Exchanging %u bookmarks, %u keys (heap: %u)", (unsigned)changeCount, (unsigned)keys.count,
          (unsigned)freeHeap);
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
    return LOW_MEMORY;
  }

  JsonBody body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId();
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";

    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    book["keysComplete"] = keys.complete;
    if (const char* sidecar = sidecarStateName(keys.sidecar)) book["sidecarState"] = sidecar;
    JsonArray jsonKeys = book["keys"].to<JsonArray>();
    for (size_t i = 0; keys.rows && i < keys.count; i++) {
      JsonObject key = jsonKeys.add<JsonObject>();
      key["k"] = keys.rows[i].k;
      key["dt"] = keys.rows[i].dt;
    }

    JsonArray jsonChanges = book["changes"].to<JsonArray>();
    for (size_t i = 0; changes && i < changeCount; i++) {
      const BookOrbitBookmark& bookmark = changes[i];
      JsonObject entry = jsonChanges.add<JsonObject>();
      entry["datetime"] = bookmark.datetime;
      entry["pos"] = bookmark.pos;
      if (!bookmark.chapter.empty()) entry["chapter"] = bookmark.chapter;
      if (bookmark.pageno > 0) entry["pageno"] = bookmark.pageno;
    }
    if (!body.build(doc)) return LOW_MEMORY;
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
  std::string response = httpCode > 0 ? http.getString().c_str() : "";
  http.end();
#else
  LOG_DBG("BookOrbit", "POST body bytes=%u", static_cast<unsigned>(body.length()));
  std::string response;
  const int httpCode = sendBookOrbitRequest("POST", url, &body, response);
#endif
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  body.release();
  LOG_DBG("BookOrbit", "Bookmark exchange response: %d", httpCode);

  if (httpCode < 0) return transportFailure();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 200 || httpCode >= 300) return SERVER_ERROR;

  JsonDocument filter;
  filter["unmatched"] = true;
  filter["results"][0]["more"] = true;
  filter["results"][0]["skippedNoPosition"] = true;
  JsonObject addFilter = filter["results"][0]["toApply"]["add"].add<JsonObject>();
  addFilter["serverId"] = true;
  addFilter["pos"] = true;
  addFilter["title"] = true;
  addFilter["pageno"] = true;
  JsonObject deleteFilter = filter["results"][0]["toApply"]["delete"].add<JsonObject>();
  deleteFilter["serverId"] = true;
  deleteFilter["key"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, response, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    LOG_ERR("BookOrbit", "Failed to parse bookmark exchange response");
    return JSON_ERROR;
  }
  for (JsonVariantConst hash : doc["unmatched"].as<JsonArrayConst>()) {
    if (documentHash == hash.as<const char*>()) {
      LOG_INF("BookOrbit", "Server does not know this document; bookmarks not exchanged");
      outUnmatched = true;
      return OK;
    }
  }
  if (outMorePending) {
    *outMorePending = (doc["results"][0]["more"] | false) || (doc["results"][0]["skippedNoPosition"] | 0) > 0;
  }
  if (!outIncoming) return OK;

  JsonObjectConst toApply = doc["results"][0]["toApply"].as<JsonObjectConst>();
  const auto collect = [&](const char* field, const bool deleted) {
    for (JsonObjectConst entry : toApply[field].as<JsonArrayConst>()) {
      if (outIncoming->size() >= BOOKORBIT_BOOKMARK_BATCH) {
        LOG_INF("BookOrbit", "More server-side bookmark changes remain; they arrive next sync");
        return;
      }
      const uint32_t serverId = entry["serverId"] | 0U;
      // Later pull rounds repeat entries not yet acknowledged.
      const bool alreadyCollected =
          std::any_of(outIncoming->begin(), outIncoming->end(),
                      [&](const BookOrbitIncomingBookmark& seen) { return seen.serverId == serverId; });
      if (alreadyCollected) continue;
      const char* pos = entry["pos"] | "";
      const char* key = entry["key"] | "";
      if (serverId == 0 || (deleted ? !*key : !*pos)) continue;

      BookOrbitIncomingBookmark incoming;
      incoming.serverId = serverId;
      incoming.key = key;
      incoming.pos = pos;
      incoming.title = entry["title"] | "";
      incoming.pageno = entry["pageno"] | 0;
      incoming.deleted = deleted;
      outIncoming->push_back(std::move(incoming));
    }
  };
  collect("add", false);
  collect("delete", true);
  if (!outIncoming->empty()) {
    LOG_INF("BookOrbit", "Server sent %u bookmark change(s)", (unsigned)outIncoming->size());
  }
  return OK;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::ackBookmarks(const std::string& documentHash,
                                                             const std::string& deviceModel,
                                                             const std::vector<BookOrbitBookmarkAck>& applied,
                                                             const std::vector<uint32_t>& deletedServerIds) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (applied.empty() && deletedServerIds.empty()) return OK;

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/bookmarks/exchange-ack";
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t heapFloor = requiredHeapFloor();
  if (freeHeap < heapFloor) {
    LOG_ERR("BookOrbit", "Insufficient heap for sync request: %u bytes free (need %u)", freeHeap, heapFloor);
    return LOW_MEMORY;
  }

  JsonBody body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId();
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = "crossink-bo-1";
    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    JsonArray appliedArray = book["applied"].to<JsonArray>();
    for (const BookOrbitBookmarkAck& ack : applied) {
      JsonObject entry = appliedArray.add<JsonObject>();
      entry["serverId"] = ack.serverId;
      entry["status"] = ack.failed ? "failed" : "applied";
      // The local identity this device minted; it is what links the server's bookmark here.
      if (!ack.failed && ack.key[0] != '\0') {
        entry["key"] = ack.key;
        entry["datetime"] = ack.datetime;
        entry["pos"] = ack.pos;
      }
    }
    JsonArray deletedArray = book["deleted"].to<JsonArray>();
    for (const uint32_t serverId : deletedServerIds) {
      JsonObject entry = deletedArray.add<JsonObject>();
      entry["serverId"] = serverId;
      entry["status"] = "applied";
    }
    if (!body.build(doc)) return LOW_MEMORY;
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
  http.end();
#else
  std::string response;
  const int httpCode = sendBookOrbitRequest("POST", url, &body, response);
#endif
  lastHttpCode = httpCode > 0 ? httpCode : 0;
  lastTransportError = httpCode < 0 ? httpCode : 0;
  LOG_INF("BookOrbit", "Bookmark ack response: %d (%u applied, %u deleted)", httpCode, (unsigned)applied.size(),
          (unsigned)deletedServerIds.size());

  if (httpCode < 0) return transportFailure();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode >= 200 && httpCode < 300) return OK;
  return SERVER_ERROR;
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
