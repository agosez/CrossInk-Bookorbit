#include <Epub.h>
#include <GfxRenderer.h>
#include <Serialization.h>

#include "Epub/Page.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"

Epub::Epub(std::string path, const std::string& cacheDir) : filepath(std::move(path)), cachePath(cacheDir) {}

const std::string& Epub::getCachePath() const { return cachePath; }
const std::string& Epub::getLanguage() const {
  static const std::string language = "en";
  return language;
}
BookMetadataCache::SpineEntry Epub::getSpineItem(int) const { return {}; }
BookMetadataCache::TocEntry Epub::getTocItem(int) const { return {}; }
int Epub::getTocItemsCount() const { return 0; }
int Epub::getTocIndexForSpineIndex(int) const { return -1; }
bool Epub::readItemContentsToStream(const std::string&, Print&, size_t, bool) const { return false; }

bool CssParser::loadFromCache() { return false; }

void Hyphenator::setPreferredLanguage(const std::string&) {}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() = default;
bool ChapterHtmlSlimParser::beginParse() { return false; }
ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() { return ParseStatus::Error; }
bool ChapterHtmlSlimParser::finishParse() { return true; }
void ChapterHtmlSlimParser::abortParse() {}
void ChapterHtmlSlimParser::releaseInputFile() {}

bool Page::serialize(FsFile& file) const {
  constexpr uint32_t marker = 0x50414745;
  return serialization::tryWritePod(file, marker);
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  uint32_t marker = 0;
  if (!serialization::tryReadPod(file, marker) || marker != 0x50414745) return nullptr;
  return std::make_unique<Page>();
}

uint16_t Page::imageEstimateUnits(uint16_t) const { return 0; }
