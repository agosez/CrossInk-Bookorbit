#include "BookOrbitCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// BookOrbit's kosync-compatible progress endpoints live under this path prefix
// (e.g. GET {server}/api/v1/koreader/users/auth), unlike a plain koreader-sync
// server which serves them at the root.
constexpr char API_PREFIX[] = "/api/v1/koreader";

// A token pasted from the Cloudflare dashboard easily carries a stray space or line break,
// which would otherwise go out inside the header line and fail the check with no clue why.
std::string trimmed(const std::string& value) {
  constexpr char WHITESPACE[] = " \t\r\n";
  const size_t first = value.find_first_not_of(WHITESPACE);
  if (first == std::string::npos) {
    return "";
  }
  return value.substr(first, value.find_last_not_of(WHITESPACE) - first + 1);
}

// The Home shelves, top then bottom, each as "kind:id:name": the id and name are empty
// but for a collection or SmartScope. Two flat strings cost the C3 far less code than
// nested JSON.
constexpr const char* HOME_SHELF_KEYS[BookOrbitCredentialStore::HOME_SHELF_COUNT] = {"homeShelfTop", "homeShelfBottom"};

// Leaves the shelf as it was when the text cannot be one: a kind from a newer firmware, or
// a list without its id.
void parseHomeShelf(const char* text, BookOrbitHomeShelfSource& shelf) {
  char* end = nullptr;
  const unsigned long kind = strtoul(text, &end, 10);
  if (end == text || *end != ':' || kind >= BOOKORBIT_HOME_SHELF_KIND_COUNT) return;
  const char* id = end + 1;
  const char* name = strchr(id, ':');
  if (name == nullptr) return;
  const auto chosen = static_cast<BookOrbitHomeShelfKind>(kind);
  const auto idLength = static_cast<size_t>(name - id);
  if (!isNamedHomeShelfKind(chosen)) {
    shelf.kind = chosen;
  } else if (idLength > 0 && idLength < sizeof(shelf.id)) {
    shelf.kind = chosen;
    memcpy(shelf.id, id, idLength);
    shelf.id[idLength] = '\0';
    snprintf(shelf.name, sizeof(shelf.name), "%s", name + 1);
  }
}
}  // namespace

void BookOrbitCredentialStore::toJson(JsonDocument& doc) const {
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);
  doc["serverUrl"] = serverUrl;
  doc["downloadFolder"] = downloadFolder;
  doc["syncBehavior"] = static_cast<uint8_t>(syncBehavior);
  doc["syncOnSleep"] = syncOnSleep;
  for (int i = 0; i < HOME_SHELF_COUNT; i++) {
    const BookOrbitHomeShelfSource& shelf = homeShelves[i];
    char value[sizeof(shelf.id) + sizeof(shelf.name) + 4];
    snprintf(value, sizeof(value), "%u:%s:%s", static_cast<unsigned>(shelf.kind), shelf.id, shelf.name);
    doc[HOME_SHELF_KEYS[i]] = value;
  }
  if (!cfClientId.empty()) {
    doc["cfClientId"] = cfClientId;
  }
  if (!cfClientSecret.empty()) {
    doc["cfClientSecret_obf"] = obfuscation::obfuscateToBase64(cfClientSecret);
  }
}

bool BookOrbitCredentialStore::fromJson(JsonVariantConst doc) {
  std::string user = doc["username"] | "";

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::INVALID && !pass.empty()) {
    LOG_ERR("BOS", "Ignoring unreadable BookOrbit password");
    pass.clear();
  }

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");
  downloadFolder = doc["downloadFolder"] | "";
  const uint8_t behavior = doc["syncBehavior"] | static_cast<uint8_t>(0);
  syncBehavior = behavior == static_cast<uint8_t>(BookOrbitSyncBehavior::SMART) ? BookOrbitSyncBehavior::SMART
                                                                                : BookOrbitSyncBehavior::ASK_EVERY_TIME;
  syncOnSleep = doc["syncOnSleep"] | false;

  // A missing or unreadable shelf is the one the Home had before shelves could be chosen.
  for (int i = 0; i < HOME_SHELF_COUNT; i++) {
    BookOrbitHomeShelfSource& shelf = homeShelves[i];
    shelf = BookOrbitHomeShelfSource{};
    shelf.kind = i == 0 ? BookOrbitHomeShelfKind::DISCOVER : BookOrbitHomeShelfKind::RECENTLY_ADDED;
    parseHomeShelf(doc[HOME_SHELF_KEYS[i]] | "", shelf);
  }

  setCfClientId(doc["cfClientId"] | "");
  // Unlike the password, this field has no legacy encoding to accept: a value that does not
  // validate is another device's file or a hand edit, and decoding it yields random bytes.
  obfuscation::DecodeStatus cfSecretStatus = obfuscation::DecodeStatus::INVALID;
  std::string cfSecret = obfuscation::deobfuscateFromBase64(doc["cfClientSecret_obf"] | "", &cfSecretStatus);
  if (cfSecretStatus != obfuscation::DecodeStatus::VALIDATED) {
    if (cfSecretStatus != obfuscation::DecodeStatus::EMPTY) {
      LOG_ERR("BOS", "Ignoring unreadable Cloudflare Access client secret");
    }
    cfSecret.clear();
  }
  setCfClientSecret(cfSecret);

  return true;
}

void BookOrbitCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  ensureLoaded();
  username = user;
  password = pass;
  LOG_DBG("BOS", "Set credentials for user: %s", user.c_str());
}

void BookOrbitCredentialStore::setCfClientId(const std::string& clientId) {
  ensureLoaded();
  cfClientId = trimmed(clientId);
  if (cfClientId.empty()) {
    cfClientSecret.clear();
  }
  LOG_DBG("BOS", "Set Cloudflare Access client ID: %s", cfClientId.empty() ? "(none)" : cfClientId.c_str());
}

void BookOrbitCredentialStore::setCfClientSecret(const std::string& clientSecret) {
  ensureLoaded();
  cfClientSecret = trimmed(clientSecret);
}

bool BookOrbitCredentialStore::sendsCfAccessToken() const {
  ensureLoaded();
  if (cfClientId.empty() || cfClientSecret.empty()) {
    return false;
  }
  // Same rule as getBaseUrl(): an address without a scheme is reached over https.
  if (serverUrl.find("://") != std::string::npos && serverUrl.rfind("https://", 0) != 0) {
    LOG_ERR("BOS", "Cloudflare Access token not sent: the server URL is not https");
    return false;
  }
  return true;
}

std::string BookOrbitCredentialStore::getMd5Password() const {
  ensureLoaded();
  if (password.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool BookOrbitCredentialStore::hasCredentials() const {
  ensureLoaded();
  return !username.empty() && !password.empty() && !serverUrl.empty();
}

void BookOrbitCredentialStore::clearCredentials() {
  ensureLoaded();
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("BOS", "Cleared BookOrbit credentials");
}

void BookOrbitCredentialStore::setServerUrl(const std::string& url) {
  ensureLoaded();
  serverUrl = url;
  LOG_DBG("BOS", "Set server URL: %s", url.empty() ? "(none)" : url.c_str());
}

std::string BookOrbitCredentialStore::getBaseUrl() const {
  ensureLoaded();
  if (serverUrl.empty()) {
    return "";
  }

  std::string url;
  if (serverUrl.find("://") == std::string::npos) {
    // Normalize URL: add https:// if no protocol specified (BookOrbit servers are self-hosted
    // and typically run behind TLS; unlike KOReaderCredentialStore we don't default to http://
    // since there's no well-known plaintext deployment convention to fall back to).
    url = "https://" + serverUrl;
  } else {
    url = serverUrl;
  }

  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  // Strip a trailing "/api/v1" or "/api/v1/koreader" the user may have pasted from
  // BookOrbit's own settings page, then append our own canonical prefix.
  constexpr char API_V1_SUFFIX[] = "/api/v1";
  if (url.size() >= sizeof(API_PREFIX) - 1 &&
      url.compare(url.size() - (sizeof(API_PREFIX) - 1), sizeof(API_PREFIX) - 1, API_PREFIX) == 0) {
    url.resize(url.size() - (sizeof(API_PREFIX) - 1));
  } else if (url.size() >= sizeof(API_V1_SUFFIX) - 1 &&
             url.compare(url.size() - (sizeof(API_V1_SUFFIX) - 1), sizeof(API_V1_SUFFIX) - 1, API_V1_SUFFIX) == 0) {
    url.resize(url.size() - (sizeof(API_V1_SUFFIX) - 1));
  }

  return url + API_PREFIX;
}
