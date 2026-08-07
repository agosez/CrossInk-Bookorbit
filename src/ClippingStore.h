#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

inline constexpr size_t CLIPPING_CHAPTER_TITLE_MAX = 48;
// Raised from 512 so long web-created highlights survive sync in full. Files written by
// older builds hold at most 512 bytes per text; older firmware reading a longer record
// treats the file as corrupt, so downgrading after saving one drops that book's clippings.
inline constexpr size_t CLIPPING_TEXT_MAX = 2048;
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 256;
inline constexpr uint16_t CLIPPING_MAX_PAGE_MATCHES = 16;

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  uint32_t layoutSignature = 0;
  uint32_t textOffset = 0;
  uint16_t textLength = 0;
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
};

struct ClippedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count = 0;
};

class ClippingStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    SaveFailed,
  };

  static ClippingStore& getInstance() { return instance; }

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                        uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                        uint16_t paragraphIndex, const std::string& text, uint32_t layoutSignature);
  bool stampMissingLayoutSignature(uint32_t layoutSignature);
  bool removeClippingAt(size_t index);
  // Replaces one clipping's stored text (capped at CLIPPING_TEXT_MAX), rewriting the file.
  // Used when the exact source text of a highlight becomes known after the clipping was built
  // from rendered words, whose reconstructed spacing can differ from the source.
  bool replaceClippingText(size_t index, const std::string& text);
  bool saveToFile();
  void clearAll();

  bool hasClippings() const { return !clippings.empty(); }
  bool hasClippingForPage(uint16_t spineIndex, uint16_t page) const;
  size_t clippingCount() const { return clippings.size(); }
  const Clipping* clippingAt(size_t index) const;
  const std::vector<Clipping>& getClippings() const { return clippings; }
  bool readClippingText(size_t index, std::string& out) const;
  bool readClippingText(const Clipping& clipping, std::string& out) const;
  // Reads at most maxBytes of the text (trimming a cut trailing UTF-8 sequence). List rows
  // show a one-line snippet, and reading a 2 KB text to display its first line -- once per
  // visible row, on every scroll step -- is what made the clippings list slow to open.
  bool readClippingTextPrefix(size_t index, size_t maxBytes, std::string& out) const;

  static bool hasAnyClippings();
  static bool getAllClippedBooks(std::vector<ClippedBookEntry>& out);
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);
  static bool migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                 const std::string& title, const std::string& author, const std::string& bookType);

 private:
  static ClippingStore instance;

  std::vector<Clipping> clippings;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool readTextSpan(const Clipping& clipping, uint16_t length, std::string& out) const;
  bool readFromFile();
  bool readFromFile(const std::string& path, std::vector<Clipping>& out) const;
  bool writeToFile(const std::string* replacementText = nullptr, size_t replacementIndex = SIZE_MAX);
};

inline bool clippingStoredRangeMatchesLayout(const Clipping& clipping, const uint16_t currentPageCount,
                                             const uint32_t currentLayoutSignature) {
  if (clipping.pageCount != currentPageCount) return false;
  // Versions 1-2 predate layout signatures. Preserve their fast path until a
  // reader relayout stamps the layout they were displayed with.
  return clipping.layoutSignature == 0 || currentLayoutSignature == 0 ||
         clipping.layoutSignature == currentLayoutSignature;
}

#define CLIPPINGS ClippingStore::getInstance()
