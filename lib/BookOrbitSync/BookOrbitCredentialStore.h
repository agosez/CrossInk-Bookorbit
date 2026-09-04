#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

/**
 * Singleton class for storing BookOrbit sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 *
 * BookOrbit is self-hosted only (no public default server), and always
 * identifies documents by the binary partial-MD5 hash (see KOReaderDocumentId),
 * so unlike KOReaderCredentialStore there is no document matching method to store.
 */
// Mirrors KOReaderSyncBehavior; declared here so this fork-owned store does
// not pull upstream's KOReader headers in.
enum class BookOrbitSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Always show the apply/upload choice screen.
  SMART = 1,           // Auto-resolve when progress order and sync dates agree.
};

class BookOrbitCredentialStore : public PersistableStore<BookOrbitCredentialStore> {
 private:
  std::string username;
  std::string password;
  std::string serverUrl;
  std::string downloadFolder;  // catalog download folder, normalized ("" = SD root)
  BookOrbitSyncBehavior syncBehavior = BookOrbitSyncBehavior::ASK_EVERY_TIME;

  BookOrbitCredentialStore() = default;
  ~BookOrbitCredentialStore() = default;

  friend class PersistableStore<BookOrbitCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookorbit.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  // Get MD5 hash of password for API authentication (BookOrbit's kosync-compatible "userkey")
  std::string getMd5Password() const;

  // Check if credentials and a server address are set
  bool hasCredentials() const;

  void setSyncBehavior(BookOrbitSyncBehavior behavior) { syncBehavior = behavior; }
  BookOrbitSyncBehavior getSyncBehavior() const { return syncBehavior; }

  // Catalog download folder. Callers must pass a normalized value: "" for the SD
  // root, otherwise "/Folder" with a leading and no trailing slash, so paths can
  // be built as folder + "/file.epub" without special-casing the root.
  void setDownloadFolder(const std::string& folder) { downloadFolder = folder; }
  const std::string& getDownloadFolder() const { return downloadFolder; }

  // Clear credentials
  void clearCredentials();

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  // Get base URL for API calls: normalized server URL + BookOrbit's kosync-compatible API prefix.
  // Empty when no server URL has been configured (BookOrbit has no public default server).
  std::string getBaseUrl() const;
};

// Helper macro to access credential store
#define BOOKORBIT_STORE BookOrbitCredentialStore::getInstance()
