#include "ReleaseJsonParser.h"

#include <cstdlib>
#include <cstring>

namespace {

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void copyGithubSha256Digest(char* dst, size_t dstSize, const char* value, size_t valueLen) {
  static constexpr char kSha256Prefix[] = "sha256:";
  static constexpr size_t kSha256PrefixLen = sizeof(kSha256Prefix) - 1;
  static constexpr size_t kSha256HexLen = 64;
  if (valueLen != kSha256PrefixLen + kSha256HexLen || memcmp(value, kSha256Prefix, kSha256PrefixLen) != 0) return;

  safeCopy(dst, dstSize, value + kSha256PrefixLen, valueLen - kSha256PrefixLen);
}

}  // namespace

ReleaseJsonParser::ReleaseJsonParser(AssetMatcher assetMatcher, ReleaseVisitor releaseVisitor, void* releaseVisitorCtx)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      assetMatcher(assetMatcher),
      releaseVisitor(releaseVisitor),
      releaseVisitorCtx(releaseVisitorCtx) {
  reset();
}

void ReleaseJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  assetDepth = 0;
  releaseList = false;
  clearRelease();
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSha256[0] = '\0';
  currentAssetSize = 0;
}

void ReleaseJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool ReleaseJsonParser::isReleaseList() const { return releaseList; }
const ReleaseJsonParser::Release& ReleaseJsonParser::release() const { return current; }
bool ReleaseJsonParser::foundTag() const { return current.tagFound; }
bool ReleaseJsonParser::foundFirmware() const { return current.firmwareFound; }
const char* ReleaseJsonParser::getTagName() const { return current.tagName; }
const char* ReleaseJsonParser::getFirmwareUrl() const { return current.firmwareUrl; }
size_t ReleaseJsonParser::getFirmwareSize() const { return current.firmwareSize; }
const char* ReleaseJsonParser::getFirmwareSha256() const { return current.firmwareSha256; }

void ReleaseJsonParser::clearRelease() {
  current.tagName[0] = '\0';
  current.firmwareUrl[0] = '\0';
  current.firmwareSha256[0] = '\0';
  current.firmwareSize = 0;
  current.tagFound = false;
  current.firmwareFound = false;
}

void ReleaseJsonParser::commitRelease() {
  if (releaseVisitor != nullptr && current.tagFound && current.firmwareFound) {
    releaseVisitor(releaseVisitorCtx, current);
  }
}

void ReleaseJsonParser::commitAsset() {
  const bool matchesFirmware =
      assetMatcher != nullptr ? assetMatcher(currentAssetName) : strcmp(currentAssetName, "firmware.bin") == 0;
  if (!current.firmwareFound && matchesFirmware) {
    memcpy(current.firmwareUrl, currentAssetUrl, sizeof(current.firmwareUrl));
    memcpy(current.firmwareSha256, currentAssetSha256, sizeof(current.firmwareSha256));
    current.firmwareSize = currentAssetSize;
    current.firmwareFound = true;
  }
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSha256[0] = '\0';
  currentAssetSize = 0;
}

// -- SAX callbacks (static trampolines) -------------------------------------

void ReleaseJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == self->releaseDepth()) {
        if (len == 8 && memcmp(key, "tag_name", 8) == 0)
          self->lastKey = LastKey::TAG_NAME;
        else if (len == 6 && memcmp(key, "assets", 6) == 0)
          self->lastKey = LastKey::ASSETS;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_ASSET_OBJECT:
      if (self->assetDepth == 1) {
        if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::ASSET_NAME;
        else if (len == 20 && memcmp(key, "browser_download_url", 20) == 0)
          self->lastKey = LastKey::ASSET_URL;
        else if (len == 4 && memcmp(key, "size", 4) == 0)
          self->lastKey = LastKey::ASSET_SIZE;
        else if (len == 6 && memcmp(key, "sha256", 6) == 0)
          self->lastKey = LastKey::ASSET_SHA256;
        else if (len == 6 && memcmp(key, "digest", 6) == 0)
          self->lastKey = LastKey::ASSET_DIGEST;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->lastKey) {
    case LastKey::TAG_NAME:
      if (self->position == Position::TOP_LEVEL && self->depth == self->releaseDepth()) {
        safeCopy(self->current.tagName, sizeof(self->current.tagName), value, len);
        self->current.tagFound = true;
      }
      break;
    case LastKey::ASSET_NAME:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetName, sizeof(self->currentAssetName), value, len);
      break;
    case LastKey::ASSET_URL:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetUrl, sizeof(self->currentAssetUrl), value, len);
      break;
    case LastKey::ASSET_SHA256:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetSha256, sizeof(self->currentAssetSha256), value, len);
      break;
    case LastKey::ASSET_DIGEST:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        copyGithubSha256Digest(self->currentAssetSha256, sizeof(self->currentAssetSha256), value, len);
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  if (self->lastKey == LastKey::ASSET_SIZE && self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
    self->currentAssetSize = static_cast<size_t>(strtoul(value, nullptr, 10));
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNull(void* ctx) { static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE; }

void ReleaseJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      // Each release of a list starts from nothing, so one without a firmware asset never
      // reports the previous release's.
      if (self->releaseList && self->depth == 2) self->clearRelease();
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::IN_ASSET_OBJECT;
      self->assetDepth = 1;
      self->currentAssetName[0] = '\0';
      self->currentAssetUrl[0] = '\0';
      self->currentAssetSha256[0] = '\0';
      self->currentAssetSize = 0;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void ReleaseJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->releaseList && self->depth == 2) self->commitRelease();
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      if (self->assetDepth == 0) {
        self->commitAsset();
        self->position = Position::IN_ASSETS_ARRAY;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::ASSETS && self->depth == self->releaseDepth()) {
        self->position = Position::IN_ASSETS_ARRAY;
      } else {
        // A document that opens with '[' is GET /releases' list of releases.
        if (self->depth == 0) self->releaseList = true;
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
