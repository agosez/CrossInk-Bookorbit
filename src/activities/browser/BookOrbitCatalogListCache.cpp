#include "BookOrbitCatalogListCache.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <functional>

namespace {
constexpr char BOOKORBIT_LIST_CACHE_DIR[] = "/.crosspoint/bookorbit_lists";
constexpr char BOOKORBIT_LIST_CACHE_PREFIX[] = "bookorbit_list_";
constexpr char BOOKORBIT_LIST_CACHE_SUFFIX[] = ".json";
constexpr size_t BOOKORBIT_CACHE_PATH_CAP = 180;
constexpr char BOOKORBIT_DOWNLOAD_RESUME_PATH[] = "/.crosspoint/bookorbit_download.json";

bool writeJsonStringFile(const std::string& path, const std::string& text) {
  FsFile file;
  if (!Storage.openFileForWrite("BookOrbit", path, file)) {
    LOG_ERR("BookOrbit", "Failed to open cache for write: %s", path.c_str());
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  const bool ok = written == text.size() && file.close();
  if (!ok) {
    LOG_ERR("BookOrbit", "Failed to write full cache file: %s", path.c_str());
    Storage.remove(path.c_str());
  }
  return ok;
}

bool readJsonStringFile(const std::string& path, std::string& out) {
  out.clear();
  FsFile file;
  if (!Storage.openFileForRead("BookOrbit", path, file)) {
    return false;
  }
  const size_t size = file.fileSize();
  if (size == 0 || size > 24576) {
    file.close();
    return false;
  }
  out.resize(size);
  const int n = file.read(out.data(), size);
  file.close();
  if (n <= 0 || static_cast<size_t>(n) != size) {
    out.clear();
    return false;
  }
  return true;
}

std::string cachePathForKey(const std::string& key) {
  const size_t hash = std::hash<std::string>{}(key);
  char path[BOOKORBIT_CACHE_PATH_CAP];
  snprintf(path, sizeof(path), "%s/%s%08lx%s", BOOKORBIT_LIST_CACHE_DIR, BOOKORBIT_LIST_CACHE_PREFIX,
           static_cast<unsigned long>(hash & 0xffffffffUL), BOOKORBIT_LIST_CACHE_SUFFIX);
  return path;
}

void appendJsonEscaped(std::string& out, const std::string& value) {
  out += '"';
  for (char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  out += '"';
}

std::string booksCacheKey(const BookOrbitBookQuery& query, const int page) {
  return std::string("books|") + std::to_string(page) + "|" + query.sort + "|" + query.readStatus + "|" + query.query +
         "|" + query.author + "|" + query.seriesId + "|" + query.series + "|" + query.collectionId + "|" +
         query.smartScopeId + "|" + query.libraryId;
}

std::string facetCacheKey(const std::string& sectionId, const int page) {
  return std::string("facet|") + sectionId + "|" + std::to_string(page);
}
}  // namespace

namespace BookOrbitCatalogListCache {

void clear() {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(BOOKORBIT_LIST_CACHE_DIR);

  FsFile dir = Storage.open(BOOKORBIT_LIST_CACHE_DIR);
  if (!dir || !dir.isDirectory()) return;

  char name[96];
  FsFile file;
  while ((file = dir.openNextFile())) {
    const size_t nameLen = file.isDirectory() ? 0 : file.getName(name, sizeof(name));
    file.close();
    if (nameLen == 0) continue;
    if (std::strncmp(name, BOOKORBIT_LIST_CACHE_PREFIX, std::strlen(BOOKORBIT_LIST_CACHE_PREFIX)) != 0) continue;
    const std::string fullPath = std::string(BOOKORBIT_LIST_CACHE_DIR) + "/" + std::string(name, nameLen);
    Storage.remove(fullPath.c_str());
  }
  dir.close();
}

bool loadRootSections(std::vector<BookOrbitCatalogSection>& outSections) {
  std::string text;
  if (!readJsonStringFile(cachePathForKey("root"), text)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok || !doc["sections"].is<JsonArray>()) return false;

  outSections.clear();
  for (JsonObjectConst section : doc["sections"].as<JsonArrayConst>()) {
    BookOrbitCatalogSection entry;
    entry.id = std::string(section["id"] | "");
    entry.title = std::string(section["title"] | "");
    outSections.push_back(std::move(entry));
  }
  return !outSections.empty();
}

void saveRootSections(const std::vector<BookOrbitCatalogSection>& sections) {
  std::string json;
  json.reserve(64 + sections.size() * 48);
  json += "{\"sections\":[";
  for (size_t i = 0; i < sections.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"id\":";
    appendJsonEscaped(json, sections[i].id);
    json += ",\"title\":";
    appendJsonEscaped(json, sections[i].title);
    json += "}";
  }
  json += "]}";
  writeJsonStringFile(cachePathForKey("root"), json);
}

bool loadCatalogCounts(BookOrbitCatalogCounts& outCounts) {
  std::string text;
  if (!readJsonStringFile(cachePathForKey("counts"), text)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok || !doc["totalBooks"].is<int>()) return false;

  outCounts = BookOrbitCatalogCounts{};
  outCounts.totalBooks = doc["totalBooks"] | -1;
  outCounts.inProgress = doc["inProgress"] | -1;
  outCounts.libraries = doc["libraries"] | -1;
  outCounts.authors = doc["authors"] | -1;
  outCounts.series = doc["series"] | -1;
  outCounts.collections = doc["collections"] | -1;
  outCounts.smartScopes = doc["smartScopes"] | -1;
  return true;
}

void saveCatalogCounts(const BookOrbitCatalogCounts& counts) {
  std::string json;
  json.reserve(160);  // seven keys plus their digits, so the append chain never reallocates
  json += "{\"totalBooks\":" + std::to_string(counts.totalBooks) +
          ",\"inProgress\":" + std::to_string(counts.inProgress) +
          ",\"libraries\":" + std::to_string(counts.libraries) + ",\"authors\":" + std::to_string(counts.authors) +
          ",\"series\":" + std::to_string(counts.series) + ",\"collections\":" + std::to_string(counts.collections) +
          ",\"smartScopes\":" + std::to_string(counts.smartScopes) + "}";
  writeJsonStringFile(cachePathForKey("counts"), json);
}

bool loadFacetPage(const std::string& sectionId, const int page, BookOrbitFacetPage& outPage) {
  std::string text;
  if (!readJsonStringFile(cachePathForKey(facetCacheKey(sectionId, page)), text)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok || !doc["items"].is<JsonArray>()) return false;

  outPage = BookOrbitFacetPage{};
  outPage.page = doc["page"] | page;
  outPage.hasNext = doc["hasNext"] | false;
  for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
    BookOrbitFacetEntry entry;
    entry.id = std::string(item["id"] | "");
    entry.title = std::string(item["title"] | "");
    entry.seriesId = std::string(item["seriesId"] | "");
    entry.count = item["count"] | 0;
    outPage.entries.push_back(std::move(entry));
  }
  return true;
}

void saveFacetPage(const std::string& sectionId, const int page, const BookOrbitFacetPage& pageData) {
  std::string json;
  json.reserve(96 + pageData.entries.size() * 80);
  json += "{\"page\":" + std::to_string(pageData.page) +
          ",\"hasNext\":" + std::string(pageData.hasNext ? "true" : "false") + ",\"items\":[";
  for (size_t i = 0; i < pageData.entries.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"id\":";
    appendJsonEscaped(json, pageData.entries[i].id);
    json += ",\"title\":";
    appendJsonEscaped(json, pageData.entries[i].title);
    json += ",\"seriesId\":";
    appendJsonEscaped(json, pageData.entries[i].seriesId);
    json += ",\"count\":" + std::to_string(pageData.entries[i].count) + "}";
  }
  json += "]}";
  writeJsonStringFile(cachePathForKey(facetCacheKey(sectionId, page)), json);
}

bool loadBooksPage(const BookOrbitBookQuery& query, const int page, BookOrbitBookPage& outPage) {
  std::string text;
  if (!readJsonStringFile(cachePathForKey(booksCacheKey(query, page)), text)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok || !doc["items"].is<JsonArray>()) return false;

  outPage = BookOrbitBookPage{};
  outPage.page = doc["page"] | page;
  outPage.total = doc["total"] | 0;
  outPage.pageSize = doc["size"] | BookOrbitCatalogClient::PAGE_SIZE;
  for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
    BookOrbitCatalogBook book;
    book.id = item["id"] | 0;
    book.title = std::string(item["title"] | "");
    book.author = std::string(item["author"] | "");
    outPage.books.push_back(std::move(book));
  }
  return true;
}

void saveBooksPage(const BookOrbitBookQuery& query, const int page, const BookOrbitBookPage& pageData) {
  std::string json;
  json.reserve(160 + pageData.books.size() * 80);
  // The filename is a hash of the key; embed the key itself so a cache file can be
  // traced back to the listing that produced it (loaders ignore the field).
  json += "{\"key\":";
  appendJsonEscaped(json, booksCacheKey(query, page));
  json += ",\"page\":" + std::to_string(pageData.page) + ",\"total\":" + std::to_string(pageData.total) +
          ",\"size\":" + std::to_string(pageData.pageSize) + ",\"items\":[";
  for (size_t i = 0; i < pageData.books.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"id\":" + std::to_string(pageData.books[i].id) + ",\"title\":";
    appendJsonEscaped(json, pageData.books[i].title);
    json += ",\"author\":";
    appendJsonEscaped(json, pageData.books[i].author);
    json += "}";
  }
  json += "]}";
  writeJsonStringFile(cachePathForKey(booksCacheKey(query, page)), json);
}

bool saveDownloadResume(const BookOrbitDownloadResume& resume) {
  std::string json;
  json.reserve(512);  // the keys alone take ~250 bytes; one allocation covers typical titles
  const auto addString = [&json](const char* key, const std::string& value) {
    json += ",\"";
    json += key;
    json += "\":";
    appendJsonEscaped(json, value);
  };
  const auto addNumber = [&json](const char* key, const long long value) {
    json += ",\"";
    json += key;
    json += "\":";
    json += std::to_string(value);
  };
  json += "{\"booksFromFacet\":";
  json += resume.booksFromFacet ? "true" : "false";
  addNumber("bookId", resume.bookId);
  addNumber("fileId", resume.fileId);
  addString("title", resume.title);
  addString("path", resume.path);
  addString("sort", resume.listQuery.sort);
  addString("readStatus", resume.listQuery.readStatus);
  addString("query", resume.listQuery.query);
  addString("author", resume.listQuery.author);
  addString("seriesId", resume.listQuery.seriesId);
  addString("series", resume.listQuery.series);
  addString("collectionId", resume.listQuery.collectionId);
  addString("smartScopeId", resume.listQuery.smartScopeId);
  addString("libraryId", resume.listQuery.libraryId);
  addString("listTitle", resume.listTitle);
  addNumber("listPage", resume.listPage);
  addNumber("selectorIndex", resume.selectorIndex);
  addNumber("topIndex", resume.topIndex);
  addString("facetSectionId", resume.facetSectionId);
  addString("facetTitle", resume.facetTitle);
  addNumber("facetPage", resume.facetPage);
  json += "}";
  return writeJsonStringFile(BOOKORBIT_DOWNLOAD_RESUME_PATH, json);
}

bool takeDownloadResume(BookOrbitDownloadResume& outResume) {
  std::string json;
  const bool read = readJsonStringFile(BOOKORBIT_DOWNLOAD_RESUME_PATH, json);
  Storage.remove(BOOKORBIT_DOWNLOAD_RESUME_PATH);
  if (!read) {
    LOG_ERR("BookOrbit", "No download to resume at %s", BOOKORBIT_DOWNLOAD_RESUME_PATH);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("BookOrbit", "Unreadable download resume file");
    return false;
  }
  const auto field = [&doc](const char* key) { return std::string(doc[key] | ""); };
  outResume = BookOrbitDownloadResume{};
  outResume.bookId = doc["bookId"] | int64_t{0};
  outResume.fileId = doc["fileId"] | int64_t{0};
  outResume.title = field("title");
  outResume.path = field("path");
  outResume.listQuery.sort = field("sort");
  outResume.listQuery.readStatus = field("readStatus");
  outResume.listQuery.query = field("query");
  outResume.listQuery.author = field("author");
  outResume.listQuery.seriesId = field("seriesId");
  outResume.listQuery.series = field("series");
  outResume.listQuery.collectionId = field("collectionId");
  outResume.listQuery.smartScopeId = field("smartScopeId");
  outResume.listQuery.libraryId = field("libraryId");
  outResume.listTitle = field("listTitle");
  outResume.listPage = doc["listPage"] | 1;
  outResume.booksFromFacet = doc["booksFromFacet"] | false;
  outResume.selectorIndex = doc["selectorIndex"] | 0;
  outResume.topIndex = doc["topIndex"] | 0;
  outResume.facetSectionId = field("facetSectionId");
  outResume.facetTitle = field("facetTitle");
  outResume.facetPage = doc["facetPage"] | 1;
  if (outResume.fileId == 0 || outResume.path.empty()) {
    LOG_ERR("BookOrbit", "Download resume file names no file to fetch");
    return false;
  }
  return true;
}

void discardDownloadResume() { Storage.remove(BOOKORBIT_DOWNLOAD_RESUME_PATH); }

}  // namespace BookOrbitCatalogListCache