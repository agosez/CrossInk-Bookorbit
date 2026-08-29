#include "BookOrbitCatalogClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitSyncClient.h"

bool BookOrbitCatalogClient::lastFetchBadResponse = false;

namespace {
// Classify a response body that failed to parse as JSON, for actionable logs.
const char* classifyBody(const char* body, size_t len) {
  size_t i = 0;
  while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) {
    i++;
  }
  if (i >= len) return "empty response";
  if (body[i] == '<') return "HTML response";
  if (body[i] != '{' && body[i] != '[') return "non-JSON response";
  return "malformed JSON";
}

std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

HttpDownloader::HeaderList authHeaders() {
  return {
      {"Accept", "application/json"},
      // We cannot decompress responses; keep proxies from picking gzip for us.
      {"Accept-Encoding", "identity"},
      {"x-auth-user", BOOKORBIT_STORE.getUsername()},
      {"x-auth-key", BOOKORBIT_STORE.getMd5Password()},
  };
}

// Minimal ArduinoJson input adapter over FsFile, which lacks Stream's readBytes().
struct FileJsonReader {
  FsFile& file;
  int read() { return file.read(); }
  size_t readBytes(char* buffer, size_t length) {
    const int n = file.read(buffer, length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
};

// Catalog responses carry full book metadata (descriptions, series, covers...) and can
// exceed the largest free heap block, especially while the TLS session is still holding
// tens of KB. Buffering them in a std::string aborts on OOM (throwing operator new with
// -fno-exceptions), so stream the body to a temp file on SD instead and parse it from
// there with a filter that keeps only the handful of fields we actually read.
bool fetchJson(const std::string& url, const JsonDocument& filter, JsonDocument& outDoc) {
  constexpr char TMP_PATH[] = "/.crosspoint/bookorbit_catalog.json";
  BookOrbitCatalogClient::lastFetchBadResponse = false;

  // Two attempts: TLS on the C3 runs with a few KB of headroom, so a fetch can fail
  // on a transient allocation race; a fresh connection usually succeeds.
  HttpDownloader::DownloadOptions options;
  // wolfSSL rather than mbedTLS: the latter needs more heap than this chip has to spare
  // while parsing a modern certificate chain, which is what makes catalog fetches fail with
  // MBEDTLS_ERR_X509_FATAL_ERROR against servers on Let's Encrypt's four-certificate path.
  options.transport = HttpDownloader::Transport::WOLFSSL;
  // Only meaningful on the esp_http_client path, kept for the fallback: body bytes arriving
  // with the headers get cached via realloc in steps of this size.
  options.clientRxBufferSize = 2048;
  HttpDownloader::DownloadError err = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < 2; attempt++) {
    options.extraHeaders = authHeaders();
    err = HttpDownloader::downloadToFile(url, TMP_PATH, nullptr, nullptr, "", "", options);
    if (err == HttpDownloader::OK) break;
    LOG_ERR("BookOrbit", "Catalog fetch attempt %d failed (err=%d, http=%d)", attempt + 1, static_cast<int>(err),
            HttpDownloader::lastHttpStatus);
  }
  if (err != HttpDownloader::OK) {
    // The server was reachable but didn't serve this path: 404/405 = endpoint missing
    // (BookOrbit version without the KOReader catalog API, or wrong server URL); a
    // final 3xx status = a proxy/SSO layer bouncing us away from the API (e.g. a
    // login redirect loop hitting the redirect limit).
    const int status = HttpDownloader::lastHttpStatus;
    if (status == 404 || status == 405 || (status >= 300 && status < 400)) {
      BookOrbitCatalogClient::lastFetchBadResponse = true;
    }
    LOG_ERR("BookOrbit", "Catalog request failed (err=%d, http=%d): %s", static_cast<int>(err), status, url.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("BookOrbit", TMP_PATH, file)) {
    LOG_ERR("BookOrbit", "Failed to reopen catalog response");
    Storage.remove(TMP_PATH);
    return false;
  }

  FileJsonReader reader{file};
  const DeserializationError error = deserializeJson(outDoc, reader, DeserializationOption::Filter(filter));

  // ArduinoJson's lenient mode parses an HTML page as a bare string and returns Ok,
  // so a parse "success" alone doesn't prove we got the catalog API: every catalog
  // endpoint returns a JSON object, and anything else is a bad response.
  const bool wrongShape = !error && !outDoc.is<JsonObject>();

  if (error || wrongShape) {
    // The server answered, but not with the catalog API's JSON. Log what it actually
    // sent (classification + preview) so a wrong server URL, a proxy serving the web
    // app, or a BookOrbit version without the catalog API is diagnosable from serial.
    BookOrbitCatalogClient::lastFetchBadResponse = true;
    char preview[97];
    size_t previewLen = 0;
    if (file.seek(0)) {
      const int n = file.read(preview, sizeof(preview) - 1);
      previewLen = n > 0 ? static_cast<size_t>(n) : 0;
    }
    for (size_t i = 0; i < previewLen; i++) {
      const char c = preview[i];
      preview[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    preview[previewLen] = '\0';
    LOG_ERR("BookOrbit", "Catalog response is not the catalog API: %s (%s, http=%d, %u bytes, preview=\"%s\")",
            error ? error.c_str() : "not a JSON object", classifyBody(preview, previewLen),
            HttpDownloader::lastHttpStatus, (unsigned)file.fileSize(), preview);
  }

  file.close();
  Storage.remove(TMP_PATH);
  return !error && !wrongShape;
}

// Sections we can browse: direct book listings plus the authors/series drill-down
// facets. BookOrbit's library/collection/smart-scope facets remain out of scope.
constexpr const char* SUPPORTED_SECTIONS[] = {
    "recent", "continue-reading", "all-books", "authors", "series",
};

bool isSupportedSection(const std::string& sectionId) {
  for (const char* id : SUPPORTED_SECTIONS) {
    if (sectionId == id) return true;
  }
  return false;
}
}  // namespace

bool BookOrbitCatalogClient::fetchRootSections(std::vector<BookOrbitCatalogSection>& outSections) {
  outSections.clear();
  if (!BOOKORBIT_STORE.hasCredentials()) return false;

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/root";
  JsonDocument filter;
  filter["sections"][0]["section"] = true;
  filter["sections"][0]["title"] = true;
  JsonDocument doc;
  if (!fetchJson(url, filter, doc)) return false;

  for (JsonObjectConst section : doc["sections"].as<JsonArrayConst>()) {
    const char* id = section["section"] | "";
    if (!isSupportedSection(id)) continue;  // skip sections we don't support browsing
    BookOrbitCatalogSection entry;
    entry.id = id;
    entry.title = std::string(section["title"] | id);
    outSections.push_back(std::move(entry));
  }
  return true;
}

bool BookOrbitCatalogClient::fetchBooks(const BookOrbitBookQuery& query, const int page, BookOrbitBookPage& outPage) {
  outPage = BookOrbitBookPage{};
  if (!BOOKORBIT_STORE.hasCredentials()) return false;

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/books?page=" + std::to_string(page) +
                    "&size=" + std::to_string(PAGE_SIZE);
  if (!query.sort.empty()) {
    url += "&sort=" + urlEncode(query.sort);
  }
  if (!query.query.empty()) {
    url += "&q=" + urlEncode(query.query);
  }
  if (!query.author.empty()) {
    url += "&author=" + urlEncode(query.author);
  }
  // Mirror BookOrbit's own plugin: prefer the numeric series id, fall back to name.
  if (!query.seriesId.empty()) {
    url += "&seriesId=" + urlEncode(query.seriesId);
  } else if (!query.series.empty()) {
    url += "&series=" + urlEncode(query.series);
  }

  JsonDocument filter;
  filter["page"] = true;
  filter["total"] = true;
  filter["size"] = true;
  filter["items"][0]["id"] = true;
  filter["items"][0]["title"] = true;
  filter["items"][0]["authors"] = true;
  JsonDocument doc;
  if (!fetchJson(url, filter, doc)) return false;

  outPage.page = doc["page"] | page;
  outPage.total = doc["total"] | 0;
  outPage.pageSize = doc["size"] | PAGE_SIZE;

  outPage.books.reserve(doc["items"].size());
  for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
    BookOrbitCatalogBook book;
    book.id = item["id"] | 0;
    book.title = std::string(item["title"] | "");
    JsonArrayConst authors = item["authors"].as<JsonArrayConst>();
    if (!authors.isNull() && authors.size() > 0) {
      book.author = std::string(authors[0].as<const char*>() ? authors[0].as<const char*>() : "");
    }
    outPage.books.push_back(std::move(book));
  }
  return true;
}

bool BookOrbitCatalogClient::fetchSectionEntries(const std::string& sectionId, const int page,
                                                 BookOrbitFacetPage& outPage) {
  outPage = BookOrbitFacetPage{};
  if (!BOOKORBIT_STORE.hasCredentials()) return false;

  std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/sections/" + urlEncode(sectionId);
  if (page > 1) {
    url += "?page=" + std::to_string(page);
  }

  JsonDocument filter;
  filter["page"] = true;
  filter["hasNext"] = true;
  filter["items"][0]["id"] = true;
  filter["items"][0]["title"] = true;
  filter["items"][0]["count"] = true;
  filter["items"][0]["seriesId"] = true;
  JsonDocument doc;
  if (!fetchJson(url, filter, doc)) return false;

  outPage.page = doc["page"] | page;
  outPage.hasNext = doc["hasNext"] | false;

  outPage.entries.reserve(doc["items"].size());
  for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
    BookOrbitFacetEntry entry;
    entry.id = std::string(item["id"] | "");
    entry.title = std::string(item["title"] | entry.id.c_str());
    entry.count = item["count"] | 0;
    // seriesId can be numeric or string depending on server version; normalize.
    if (!item["seriesId"].isNull()) {
      if (item["seriesId"].is<const char*>()) {
        entry.seriesId = std::string(item["seriesId"] | "");
      } else {
        entry.seriesId = std::to_string(item["seriesId"] | 0LL);
      }
    }
    outPage.entries.push_back(std::move(entry));
  }
  return true;
}

bool BookOrbitCatalogClient::fetchBookDetail(const int64_t bookId, BookOrbitBookDetail& outDetail) {
  outDetail = BookOrbitBookDetail{};
  if (!BOOKORBIT_STORE.hasCredentials()) return false;

  // BookOrbit's own KOReader plugin always sends a deviceId with detail requests
  // (the server uses it for per-device read state); mirror that with the same
  // per-reader id BookOrbitSyncClient reports as device_id in progress updates.
  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/books/" + std::to_string(bookId) +
                          "?deviceId=" + BookOrbitSyncClient::deviceId();
  JsonDocument filter;
  filter["id"] = true;
  filter["title"] = true;
  filter["authors"] = true;
  filter["files"][0]["id"] = true;
  filter["files"][0]["format"] = true;
  filter["files"][0]["sizeBytes"] = true;
  JsonDocument doc;
  if (!fetchJson(url, filter, doc)) return false;

  outDetail.id = doc["id"] | bookId;
  outDetail.title = std::string(doc["title"] | "");
  JsonArrayConst authors = doc["authors"].as<JsonArrayConst>();
  if (!authors.isNull() && authors.size() > 0) {
    outDetail.author = std::string(authors[0].as<const char*>() ? authors[0].as<const char*>() : "");
  }

  outDetail.files.reserve(doc["files"].size());
  for (JsonObjectConst file : doc["files"].as<JsonArrayConst>()) {
    BookOrbitCatalogFile entry;
    entry.id = file["id"] | 0;
    entry.format = std::string(file["format"] | "");
    entry.sizeBytes = file["sizeBytes"] | 0;
    outDetail.files.push_back(std::move(entry));
  }
  return true;
}

HttpDownloader::DownloadError BookOrbitCatalogClient::downloadFile(const int64_t fileId, const std::string& destPath,
                                                                   HttpDownloader::ProgressCallback progress,
                                                                   bool* cancelFlag,
                                                                   HttpDownloader::DownloadOptions options) {
  if (!BOOKORBIT_STORE.hasCredentials()) return HttpDownloader::HTTP_ERROR;

  const std::string url =
      BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/files/" + std::to_string(fileId) + "/download";
  options.extraHeaders = authHeaders();
  // Same transport and trust as the JSON endpoints: this request carries the same
  // credentials, and mbedTLS cannot complete the handshake on this hardware.
  options.transport = HttpDownloader::Transport::WOLFSSL;
  return HttpDownloader::downloadToFile(url, destPath, std::move(progress), cancelFlag, "", "", std::move(options));
}
