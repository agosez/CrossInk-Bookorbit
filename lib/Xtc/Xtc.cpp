/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t BMP_2BIT_HEADER_SIZE = 70;
constexpr uint8_t XTH_TO_BMP[4] = {3, 1, 2, 0};
constexpr uint8_t XTH_TO_GRAY[4] = {255, 85, 170, 0};

void writeLe16(uint8_t* out, const uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* out, const uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

void create2BitBmpHeader(uint8_t (&header)[BMP_2BIT_HEADER_SIZE], const uint16_t width, const uint16_t height,
                         const uint32_t rowSize) {
  memset(header, 0, sizeof(header));
  const uint32_t imageSize = rowSize * height;
  header[0] = 'B';
  header[1] = 'M';
  writeLe32(header + 2, static_cast<uint32_t>(sizeof(header)) + imageSize);
  writeLe32(header + 10, sizeof(header));
  writeLe32(header + 14, 40);  // BITMAPINFOHEADER
  writeLe32(header + 18, width);
  writeLe32(header + 22, static_cast<uint32_t>(-static_cast<int32_t>(height)));
  writeLe16(header + 26, 1);
  writeLe16(header + 28, 2);
  writeLe32(header + 34, imageSize);
  writeLe32(header + 38, 2835);
  writeLe32(header + 42, 2835);
  writeLe32(header + 46, 4);
  writeLe32(header + 50, 4);
  for (uint8_t i = 0; i < 4; ++i) {
    const uint8_t gray = static_cast<uint8_t>(i * 85);
    const size_t paletteOffset = 54 + static_cast<size_t>(i) * 4;
    header[paletteOffset] = gray;
    header[paletteOffset + 1] = gray;
    header[paletteOffset + 2] = gray;
  }
}

void yieldDuringThumbnail(uint8_t& rowsSinceYield) {
  if (++rowsSinceYield < 8) return;
  rowsSinceYield = 0;
  vTaskDelay(1);
}

constexpr char XTCH_SOURCE_CACHE_NAME[] = "cover_src_xtch_v1.bin";
constexpr char XTCH_SOURCE_CACHE_TMP_NAME[] = "cover_src_xtch_v1.tmp";
constexpr uint8_t XTCH_SOURCE_CACHE_SCHEMA = 1;
constexpr size_t XTCH_STREAM_CHUNK_SIZE = 1024;
constexpr uint16_t XTCH_SOURCE_BAND_ROWS = 8;

struct __attribute__((packed)) XtchSourceCacheHeader {
  char magic[4];
  uint8_t schema;
  uint8_t bitDepth;
  uint16_t width;
  uint16_t height;
  uint16_t rowBytes;
  uint32_t payloadBytes;
};
static_assert(sizeof(XtchSourceCacheHeader) == 16);

bool writeExact(FsFile& file, const uint8_t* data, const size_t size) { return file.write(data, size) == size; }

bool readExact(FsFile& file, uint8_t* data, const size_t size) { return file.read(data, size) == size; }

std::string xtchSourceCachePath(const Xtc& xtc) { return xtc.getCachePath() + "/" + XTCH_SOURCE_CACHE_NAME; }

bool hasValidXtchSourceCache(const std::string& path, const xtc::PageInfo& pageInfo, const size_t rowBytes,
                             const size_t payloadBytes) {
  FsFile source;
  if (!Storage.openFileForRead("XTC", path, source)) return false;

  XtchSourceCacheHeader header{};
  const bool valid = readExact(source, reinterpret_cast<uint8_t*>(&header), sizeof(header)) &&
                     memcmp(header.magic, "XCS1", sizeof(header.magic)) == 0 &&
                     header.schema == XTCH_SOURCE_CACHE_SCHEMA && header.bitDepth == 2 &&
                     header.width == pageInfo.width && header.height == pageInfo.height &&
                     header.rowBytes == rowBytes && header.payloadBytes == payloadBytes &&
                     source.size() == sizeof(header) + payloadBytes;
  source.close();
  return valid;
}

bool ensureXtchSourceCache(const Xtc& xtc, const xtc::PageInfo& pageInfo, std::string& sourcePath) {
  const size_t rowBytes = (static_cast<size_t>(pageInfo.width) + 3) / 4;
  const size_t payloadBytes = rowBytes * pageInfo.height;
  if (rowBytes * XTCH_SOURCE_BAND_ROWS > XTCH_STREAM_CHUNK_SIZE) {
    LOG_ERR("XTC", "XTCH source row is too wide for bounded conversion");
    return false;
  }
  sourcePath = xtchSourceCachePath(xtc);
  if (hasValidXtchSourceCache(sourcePath, pageInfo, rowBytes, payloadBytes)) return true;

  if (Storage.exists(sourcePath.c_str()) && !Storage.remove(sourcePath.c_str())) {
    LOG_ERR("XTC", "Failed to remove stale XTCH source cache");
    return false;
  }

  const std::string tmpPath = xtc.getCachePath() + "/" + XTCH_SOURCE_CACHE_TMP_NAME;
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  FsFile output;
  if (!Storage.openFileForWrite("XTC", tmpPath, output)) {
    LOG_ERR("XTC", "Failed to create XTCH source cache");
    return false;
  }

  XtchSourceCacheHeader header{{'X', 'C', 'S', '1'},
                               XTCH_SOURCE_CACHE_SCHEMA,
                               2,
                               pageInfo.width,
                               pageInfo.height,
                               static_cast<uint16_t>(rowBytes),
                               static_cast<uint32_t>(payloadBytes)};
  bool success = writeExact(output, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  // Eight packed rows are at most 960 bytes on current 480px-wide XTC pages. Heap storage is
  // used because this helper runs through the parser callback, where task stack headroom is limited.
  auto band = makeUniqueNoThrow<uint8_t[]>(rowBytes * XTCH_SOURCE_BAND_ROWS);
  if (!band) {
    LOG_ERR("XTC", "Failed to allocate XTCH source band (%u bytes)",
            static_cast<unsigned int>(rowBytes * XTCH_SOURCE_BAND_ROWS));
    success = false;
  }

  const size_t colBytes = (pageInfo.height + 7) / 8;
  const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
  uint8_t rowsSinceYield = 0;
  for (uint16_t bandY = 0; success && bandY < pageInfo.height; bandY += XTCH_SOURCE_BAND_ROWS) {
    const uint16_t bandHeight = std::min<uint16_t>(XTCH_SOURCE_BAND_ROWS, pageInfo.height - bandY);
    memset(band.get(), 0, rowBytes * XTCH_SOURCE_BAND_ROWS);
    const xtc::XtcError error = xtc.loadPageStreaming(
        0,
        [&](const uint8_t* data, const size_t size, const size_t offset) {
          for (size_t i = 0; i < size; ++i) {
            const size_t absoluteOffset = offset + i;
            const bool secondPlane = absoluteOffset >= planeSize;
            const size_t planeOffset = secondPlane ? absoluteOffset - planeSize : absoluteOffset;
            const size_t colIndex = planeOffset / colBytes;
            if (colIndex >= pageInfo.width) continue;
            const uint16_t y = static_cast<uint16_t>((planeOffset % colBytes) * 8);
            for (uint8_t bit = 0; bit < 8 && y + bit < pageInfo.height; ++bit) {
              const uint16_t pixelY = static_cast<uint16_t>(y + bit);
              if (pixelY < bandY || pixelY >= bandY + bandHeight) continue;
              if (((data[i] >> (7 - bit)) & 1) == 0) continue;
              const uint16_t x = static_cast<uint16_t>(pageInfo.width - 1 - colIndex);
              const size_t packedOffset = static_cast<size_t>(pixelY - bandY) * rowBytes + x / 4;
              const uint8_t shift = static_cast<uint8_t>(6 - (x % 4) * 2);
              band[packedOffset] |= static_cast<uint8_t>((secondPlane ? 1 : 2) << shift);
            }
          }
        },
        XTCH_STREAM_CHUNK_SIZE);
    if (error != xtc::XtcError::OK || !writeExact(output, band.get(), rowBytes * bandHeight)) {
      LOG_ERR("XTC", "Failed to build XTCH source cache");
      success = false;
      break;
    }
    yieldDuringThumbnail(rowsSinceYield);
  }

  const bool closed = output.close();
  if (!success || !closed || !Storage.rename(tmpPath.c_str(), sourcePath.c_str())) {
    if (success) LOG_ERR("XTC", "Failed to finalize XTCH source cache");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool openXtchSourceCache(const Xtc& xtc, const xtc::PageInfo& pageInfo, FsFile& source, size_t& rowBytes) {
  std::string sourcePath;
  if (!ensureXtchSourceCache(xtc, pageInfo, sourcePath)) return false;
  rowBytes = (static_cast<size_t>(pageInfo.width) + 3) / 4;
  if (!Storage.openFileForRead("XTC", sourcePath, source) || !source.seek(sizeof(XtchSourceCacheHeader))) {
    LOG_ERR("XTC", "Failed to open XTCH source cache");
    source.close();
    return false;
  }
  return true;
}

bool readXtchSourceRow(FsFile& source, const size_t rowBytes, const uint16_t y, uint8_t* row) {
  const uint32_t offset = sizeof(XtchSourceCacheHeader) + static_cast<uint32_t>(y) * rowBytes;
  return source.seek(offset) && readExact(source, row, rowBytes);
}

bool replaceGeneratedBmp(const std::string& tmpPath, const std::string& finalPath) {
  if (Storage.exists(finalPath.c_str()) && !Storage.remove(finalPath.c_str())) {
    LOG_ERR("XTC", "Failed to replace generated BMP");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("XTC", "Failed to finalize generated BMP");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}
}  // namespace

namespace {
bool thumbnailHasDimensions(const std::string& path, const uint16_t width, const uint16_t height) {
  FsFile file;
  if (!Storage.openFileForRead("XTC", path, file)) {
    return false;
  }

  Bitmap bitmap(file);
  const bool matches =
      bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() == width && bitmap.getHeight() == height;
  file.close();
  return matches;
}
}  // namespace

bool Xtc::load() {
  // Initialize parser
  parser.reset(new xtc::XtcParser());

  // Open XTC file
  xtc::XtcError err = parser->open(filepath.c_str());
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }

  loaded = true;
  return true;
}

bool Xtc::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to clear cache");
    return false;
  }

  return true;
}

void Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // Create directories recursively
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Xtc::getTitle() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get title from XTC metadata first
  std::string title = parser->getTitle();
  if (!title.empty()) {
    return title;
  }

  // Fallback: extract filename from path as title
  size_t lastSlash = filepath.find_last_of('/');
  size_t lastDot = filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    return filepath.substr(lastSlash);
  }

  return filepath.substr(lastSlash, lastDot - lastSlash);
}

std::string Xtc::getAuthor() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get author from XTC metadata
  return parser->getAuthor();
}

bool Xtc::hasChapters() const {
  if (!loaded || !parser) {
    return false;
  }
  return parser->hasChapters();
}

size_t Xtc::getChapterCount() const {
  return loaded && parser ? const_cast<xtc::XtcParser*>(parser.get())->getChapterCount() : 0;
}

size_t Xtc::getChapters(const size_t firstIndex, xtc::ChapterInfo* chapters, const size_t capacity) const {
  return loaded && parser ? const_cast<xtc::XtcParser*>(parser.get())->getChapters(firstIndex, chapters, capacity) : 0;
}

bool Xtc::getChapter(const size_t index, xtc::ChapterInfo& chapter) const {
  return loaded && parser && const_cast<xtc::XtcParser*>(parser.get())->getChapter(index, chapter);
}

bool Xtc::getChapterForPage(const uint32_t page, xtc::ChapterInfo& chapter, size_t* chapterIndex) const {
  return loaded && parser && const_cast<xtc::XtcParser*>(parser.get())->getChapterForPage(page, chapter, chapterIndex);
}

std::string Xtc::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Xtc::generateCoverBmp() const {
  const std::string coverPath = getCoverBmpPath();
  const bool coverExists = Storage.exists(coverPath.c_str());

  if (!loaded || !parser) {
    if (coverExists) return true;
    LOG_ERR("XTC", "Cannot generate cover BMP, file not loaded");
    return false;
  }

  const uint8_t bitDepth = parser->getBitDepth();
  if (coverExists) {
    if (bitDepth != 2) return true;

    FsFile existing;
    bool alreadyTwoBit = false;
    if (Storage.openFileForRead("XTC", coverPath, existing)) {
      {
        Bitmap bitmap(existing);
        alreadyTwoBit = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getBpp() == 2;
      }
      existing.close();
    }
    if (alreadyTwoBit) return true;
    if (!Storage.remove(coverPath.c_str())) {
      LOG_ERR("XTC", "Failed to replace legacy XTCH cover BMP");
      return false;
    }
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  if (bitDepth == 2) {
    FsFile source;
    size_t sourceRowBytes = 0;
    if (!openXtchSourceCache(*this, pageInfo, source, sourceRowBytes)) return false;

    const uint32_t dstRowSize = ((static_cast<uint32_t>(pageInfo.width) * 2 + 31) / 32) * 4;
    // The output row is at most 200 bytes for the current 800px display. Heap storage avoids
    // growing this cold-path task stack and is reused for every row.
    auto sourceRow = makeUniqueNoThrow<uint8_t[]>(sourceRowBytes);
    auto outputRow = makeUniqueNoThrow<uint8_t[]>(dstRowSize);
    if (!sourceRow || !outputRow) {
      LOG_ERR("XTC", "Failed to allocate XTCH cover rows (%u + %u bytes)", static_cast<unsigned int>(sourceRowBytes),
              static_cast<unsigned int>(dstRowSize));
      source.close();
      return false;
    }

    const std::string tmpPath = coverPath + ".tmp";
    if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
    FsFile coverBmp;
    if (!Storage.openFileForWrite("XTC", tmpPath, coverBmp)) {
      LOG_ERR("XTC", "Failed to create XTCH cover BMP");
      source.close();
      return false;
    }

    uint8_t bmpHeader[BMP_2BIT_HEADER_SIZE];
    create2BitBmpHeader(bmpHeader, pageInfo.width, pageInfo.height, dstRowSize);
    bool success = writeExact(coverBmp, bmpHeader, sizeof(bmpHeader));
    // XTH pixel values use 0=white, 1=dark gray, 2=light gray, 3=black.
    // The BMP palette is ordered black through white.
    for (uint16_t y = 0; success && y < pageInfo.height; ++y) {
      if (!readXtchSourceRow(source, sourceRowBytes, y, sourceRow.get())) {
        LOG_ERR("XTC", "Failed to read XTCH cover source row");
        success = false;
        break;
      }
      uint8_t* dst = outputRow.get();
      memset(dst, 0, dstRowSize);
      for (uint16_t x = 0; x < pageInfo.width; ++x) {
        const uint8_t pixel = static_cast<uint8_t>((sourceRow[x / 4] >> (6 - (x % 4) * 2)) & 0x03);
        dst[x / 4] |= static_cast<uint8_t>(XTH_TO_BMP[pixel] << (6 - (x % 4) * 2));
      }
      success = writeExact(coverBmp, dst, dstRowSize);
    }
    const bool sourceClosed = source.close();
    const bool bmpClosed = coverBmp.close();
    if (!success || !sourceClosed || !bmpClosed) {
      if (success) LOG_ERR("XTC", "Failed to close XTCH cover BMP");
      Storage.remove(tmpPath.c_str());
      return false;
    }
    return replaceGeneratedBmp(tmpPath, coverPath);
  }

  // Allocate buffer for page data
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t bitmapSize;
  bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  // Load first page (cover)
  size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead == 0) {
    LOG_ERR("XTC", "Failed to load cover page");
    free(pageBuffer);
    return false;
  }

  // Create BMP file
  FsFile coverBmp;
  if (!Storage.openFileForWrite("XTC", getCoverBmpPath(), coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP file");
    free(pageBuffer);
    return false;
  }

  // Write 1-bit BMP header (top-down row order)
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, pageInfo.width, pageInfo.height, BmpRowOrder::TopDown);
  coverBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = ((pageInfo.width + 31) / 32) * 4;

  // Write bitmap data
  // BMP requires 4-byte row alignment
  const size_t dstRowSize = (pageInfo.width + 7) / 8;  // 1-bit destination row size

  // 1-bit source: write directly with proper padding
  const size_t srcRowSize = (pageInfo.width + 7) / 8;

  for (uint16_t y = 0; y < pageInfo.height; y++) {
    // Write source row
    coverBmp.write(pageBuffer + y * srcRowSize, srcRowSize);

    // Pad to 4-byte boundary
    uint8_t padding[4] = {0, 0, 0, 0};
    size_t paddingSize = rowSize - srcRowSize;
    if (paddingSize > 0) {
      coverBmp.write(padding, paddingSize);
    }
  }

  free(pageBuffer);

  return true;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(uint16_t height) const {
  // Height-only home themes resolve this template with the shared 2:3 cover ratio.
  const uint16_t width = static_cast<uint16_t>((static_cast<uint32_t>(height) * 2 + 1) / 3);
  const std::string newPath = getThumbBmpPath(width, height);
  if (Storage.exists(newPath.c_str())) {
    return newPath;
  }

  const std::string legacyPath = cachePath + "/thumb_" + std::to_string(height) + ".bmp";
  if (Storage.exists(legacyPath.c_str())) {
    return legacyPath;
  }

  return newPath;
}
std::string Xtc::getThumbBmpPath(uint16_t width, uint16_t height) const {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

bool Xtc::generateThumbBmp() const {
  const uint16_t height = getPageHeight();
  return height > 0 && generateThumbBmp(height);
}

bool Xtc::generateThumbBmp(uint16_t height) const {
  const uint16_t width = static_cast<uint16_t>((static_cast<uint32_t>(height) * 2 + 1) / 3);
  return generateThumbBmp(width, height);
}

bool Xtc::generateThumbBmp(uint16_t width, uint16_t height) const {
  if (width == 0 || height == 0) {
    LOG_ERR("XTC", "Cannot generate thumb BMP with invalid dimensions: %ux%u", width, height);
    return false;
  }
  const std::string thumbPath = getThumbBmpPath(width, height);
  const bool thumbExists = Storage.exists(thumbPath.c_str());
  if (thumbExists) {
    if (thumbnailHasDimensions(thumbPath, width, height)) {
      return true;
    }
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
    return false;
  }
  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  setupCacheDir();

  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }
  if (pageInfo.width == 0 || pageInfo.height == 0) {
    LOG_ERR("XTC", "Cannot generate thumb BMP with invalid page dimensions: %ux%u", pageInfo.width, pageInfo.height);
    return false;
  }
  if (thumbExists) {
    Storage.remove(thumbPath.c_str());
  }

  const uint8_t bitDepth = parser->getBitDepth();
  const uint16_t THUMB_TARGET_WIDTH = width;
  const uint16_t THUMB_TARGET_HEIGHT = height;

  const float scaleX = static_cast<float>(THUMB_TARGET_WIDTH) / pageInfo.width;
  const float scaleY = static_cast<float>(THUMB_TARGET_HEIGHT) / pageInfo.height;
  const float scale = std::max(scaleX, scaleY);
  const uint16_t thumbWidth = THUMB_TARGET_WIDTH;
  const uint16_t thumbHeight = THUMB_TARGET_HEIGHT;

  if (bitDepth == 2) {
    FsFile source;
    size_t sourceRowBytes = 0;
    if (!openXtchSourceCache(*this, pageInfo, source, sourceRowBytes)) return false;

    const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
    // These reusable buffers are bounded by the requested thumb width (currently <=340px):
    // source row <=120 bytes, BMP row ~=44 bytes, and 4 bytes per destination pixel for sums.
    auto sourceRow = makeUniqueNoThrow<uint8_t[]>(sourceRowBytes);
    auto outputRow = makeUniqueNoThrow<uint8_t[]>(rowSize);
    auto graySums = makeUniqueNoThrow<uint32_t[]>(thumbWidth);
    if (!sourceRow || !outputRow || !graySums) {
      LOG_ERR("XTC", "Failed to allocate XTCH thumbnail rows (%u, %u, %u bytes)",
              static_cast<unsigned int>(sourceRowBytes), static_cast<unsigned int>(rowSize),
              static_cast<unsigned int>(sizeof(uint32_t) * thumbWidth));
      source.close();
      return false;
    }

    const std::string tmpPath = thumbPath + ".tmp";
    if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
    FsFile thumbBmp;
    if (!Storage.openFileForWrite("XTC", tmpPath, thumbBmp)) {
      LOG_ERR("XTC", "Failed to create XTCH thumbnail BMP");
      source.close();
      return false;
    }

    BmpHeader bmpHeader;
    createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
    const uint32_t scaleInv_fp = static_cast<uint32_t>(65536.0f / scale);
    const uint64_t srcWidth_fp = static_cast<uint64_t>(pageInfo.width) << 16;
    const uint64_t srcHeight_fp = static_cast<uint64_t>(pageInfo.height) << 16;
    const uint64_t visibleWidth_fp = static_cast<uint64_t>(thumbWidth) * scaleInv_fp;
    const uint64_t visibleHeight_fp = static_cast<uint64_t>(thumbHeight) * scaleInv_fp;
    const uint32_t cropX_fp =
        static_cast<uint32_t>(srcWidth_fp > visibleWidth_fp ? (srcWidth_fp - visibleWidth_fp) / 2 : 0);
    const uint32_t cropY_fp =
        static_cast<uint32_t>(srcHeight_fp > visibleHeight_fp ? (srcHeight_fp - visibleHeight_fp) / 2 : 0);
    bool success = writeExact(thumbBmp, reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));
    uint8_t rowsSinceYield = 0;

    for (uint16_t dstY = 0; success && dstY < thumbHeight; ++dstY) {
      uint32_t srcYStart = (cropY_fp + static_cast<uint32_t>(dstY) * scaleInv_fp) >> 16;
      uint32_t srcYEnd = (cropY_fp + static_cast<uint32_t>(dstY + 1) * scaleInv_fp) >> 16;
      if (srcYStart >= pageInfo.height) srcYStart = pageInfo.height - 1;
      if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;
      if (srcYEnd <= srcYStart) srcYEnd = srcYStart + 1;
      if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;

      memset(graySums.get(), 0, sizeof(uint32_t) * thumbWidth);
      for (uint32_t srcY = srcYStart; srcY < srcYEnd; ++srcY) {
        if (!readXtchSourceRow(source, sourceRowBytes, static_cast<uint16_t>(srcY), sourceRow.get())) {
          LOG_ERR("XTC", "Failed to read XTCH thumbnail source row");
          success = false;
          break;
        }
        for (uint16_t dstX = 0; dstX < thumbWidth; ++dstX) {
          uint32_t srcXStart = (cropX_fp + static_cast<uint32_t>(dstX) * scaleInv_fp) >> 16;
          uint32_t srcXEnd = (cropX_fp + static_cast<uint32_t>(dstX + 1) * scaleInv_fp) >> 16;
          if (srcXStart >= pageInfo.width) srcXStart = pageInfo.width - 1;
          if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
          if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
          if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
          for (uint32_t srcX = srcXStart; srcX < srcXEnd; ++srcX) {
            const uint8_t pixel = static_cast<uint8_t>((sourceRow[srcX / 4] >> (6 - (srcX % 4) * 2)) & 0x03);
            graySums[dstX] += XTH_TO_GRAY[pixel];
          }
        }
      }

      memset(outputRow.get(), 0xFF, rowSize);
      for (uint16_t dstX = 0; success && dstX < thumbWidth; ++dstX) {
        uint32_t srcXStart = (cropX_fp + static_cast<uint32_t>(dstX) * scaleInv_fp) >> 16;
        uint32_t srcXEnd = (cropX_fp + static_cast<uint32_t>(dstX + 1) * scaleInv_fp) >> 16;
        if (srcXStart >= pageInfo.width) srcXStart = pageInfo.width - 1;
        if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
        if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
        if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
        const uint32_t totalCount = (srcYEnd - srcYStart) * (srcXEnd - srcXStart);
        const uint8_t avgGray = totalCount > 0 ? static_cast<uint8_t>(graySums[dstX] / totalCount) : 255;
        uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u + static_cast<uint32_t>(dstY) * 668265263u;
        hash = (hash ^ (hash >> 13)) * 1274126177u;
        const int adjustedThreshold = 128 + ((static_cast<int>(hash >> 24) - 128) / 2);
        if (avgGray < adjustedThreshold) outputRow[dstX / 8] &= static_cast<uint8_t>(~(1 << (7 - (dstX % 8))));
      }
      success = success && writeExact(thumbBmp, outputRow.get(), rowSize);
      yieldDuringThumbnail(rowsSinceYield);
    }

    const bool sourceClosed = source.close();
    const bool bmpClosed = thumbBmp.close();
    if (!success || !sourceClosed || !bmpClosed) {
      if (success) LOG_ERR("XTC", "Failed to close XTCH thumbnail BMP");
      Storage.remove(tmpPath.c_str());
      return false;
    }
    return replaceGeneratedBmp(tmpPath, thumbPath);
  }

  size_t bitmapSize;
  bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead == 0) {
    LOG_ERR("XTC", "Failed to load cover page for thumb");
    free(pageBuffer);
    return false;
  }

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", thumbPath, thumbBmp)) {
    free(pageBuffer);
    return false;
  }

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(BmpHeader));

  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowSize));
  if (!rowBuffer) {
    free(pageBuffer);
    thumbBmp.close();
    Storage.remove(thumbPath.c_str());
    return false;
  }

  const uint32_t scaleInv_fp = static_cast<uint32_t>(65536.0f / scale);
  const uint64_t srcWidth_fp = static_cast<uint64_t>(pageInfo.width) << 16;
  const uint64_t srcHeight_fp = static_cast<uint64_t>(pageInfo.height) << 16;
  const uint64_t visibleWidth_fp = static_cast<uint64_t>(thumbWidth) * scaleInv_fp;
  const uint64_t visibleHeight_fp = static_cast<uint64_t>(thumbHeight) * scaleInv_fp;
  const uint32_t cropX_fp =
      static_cast<uint32_t>(srcWidth_fp > visibleWidth_fp ? (srcWidth_fp - visibleWidth_fp) / 2 : 0);
  const uint32_t cropY_fp =
      static_cast<uint32_t>(srcHeight_fp > visibleHeight_fp ? (srcHeight_fp - visibleHeight_fp) / 2 : 0);
  const size_t planeSize = (bitDepth == 2) ? ((static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8) : 0;
  const uint8_t* plane1 = (bitDepth == 2) ? pageBuffer : nullptr;
  const uint8_t* plane2 = (bitDepth == 2) ? pageBuffer + planeSize : nullptr;
  const size_t colBytes = (bitDepth == 2) ? ((pageInfo.height + 7) / 8) : 0;
  const size_t srcRowBytes = (bitDepth == 1) ? ((pageInfo.width + 7) / 8) : 0;
  uint8_t rowsSinceYield = 0;

  for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
    memset(rowBuffer, 0xFF, rowSize);
    uint32_t srcYStart = (cropY_fp + static_cast<uint32_t>(dstY) * scaleInv_fp) >> 16;
    uint32_t srcYEnd = (cropY_fp + static_cast<uint32_t>(dstY + 1) * scaleInv_fp) >> 16;
    if (srcYStart >= pageInfo.height) srcYStart = pageInfo.height - 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;
    if (srcYEnd <= srcYStart) srcYEnd = srcYStart + 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;

    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      uint32_t srcXStart = (cropX_fp + static_cast<uint32_t>(dstX) * scaleInv_fp) >> 16;
      uint32_t srcXEnd = (cropX_fp + static_cast<uint32_t>(dstX + 1) * scaleInv_fp) >> 16;
      if (srcXStart >= pageInfo.width) srcXStart = pageInfo.width - 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
      if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;

      uint32_t graySum = 0, totalCount = 0;
      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          uint8_t grayValue = 255;
          if (bitDepth == 2) {
            if (srcX < pageInfo.width) {
              const size_t colIndex = pageInfo.width - 1 - srcX;
              const size_t byteInCol = srcY / 8;
              const size_t bitInByte = 7 - (srcY % 8);
              const size_t byteOffset = colIndex * colBytes + byteInCol;
              if (byteOffset < planeSize) {
                const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
                const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
                grayValue = XTH_TO_GRAY[(bit1 << 1) | bit2];
              }
            }
          } else {
            const size_t byteIdx = srcY * srcRowBytes + srcX / 8;
            const size_t bitIdx = 7 - (srcX % 8);
            if (byteIdx < bitmapSize) {
              grayValue = ((pageBuffer[byteIdx] >> bitIdx) & 1) ? 255 : 0;
            }
          }
          graySum += grayValue;
          totalCount++;
        }
      }

      uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;
      uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u + static_cast<uint32_t>(dstY) * 668265263u;
      hash = (hash ^ (hash >> 13)) * 1274126177u;
      const int threshold = static_cast<int>(hash >> 24);
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);
      uint8_t oneBit = (avgGray >= adjustedThreshold) ? 1 : 0;
      const size_t byteIndex = dstX / 8;
      const size_t bitOffset = 7 - (dstX % 8);
      if (byteIndex < rowSize) {
        if (!oneBit) {
          rowBuffer[byteIndex] &= ~(1 << bitOffset);
        }
      }
    }
    thumbBmp.write(rowBuffer, rowSize);
    yieldDuringThumbnail(rowsSinceYield);
  }

  free(rowBuffer);
  thumbBmp.close();
  free(pageBuffer);
  return true;
}

uint32_t Xtc::getPageCount() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getPageCount();
}

uint16_t Xtc::getPageWidth() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getWidth();
}

uint16_t Xtc::getPageHeight() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getHeight();
}

uint8_t Xtc::getBitDepth() const {
  if (!loaded || !parser) {
    return 1;  // Default to 1-bit
  }
  return parser->getBitDepth();
}

size_t Xtc::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPage(pageIndex, buffer, bufferSize);
}

xtc::XtcError Xtc::loadPageStreaming(uint32_t pageIndex,
                                     std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                     size_t chunkSize) const {
  if (!loaded || !parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(pageIndex, callback, chunkSize);
}

uint8_t Xtc::calculateProgress(uint32_t currentPage) const {
  if (!loaded || !parser || parser->getPageCount() == 0) {
    return 0;
  }
  return static_cast<uint8_t>((currentPage + 1) * 100 / parser->getPageCount());
}

xtc::XtcError Xtc::getLastError() const {
  if (!parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return parser->getLastError();
}
