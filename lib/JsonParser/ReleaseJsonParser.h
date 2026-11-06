#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

class ReleaseJsonParser {
 public:
  using AssetMatcher = bool (*)(const char* assetName);

  // One release as read from the response: its tag, and the first firmware asset
  // assetMatcher accepted.
  struct Release {
    char tagName[32];
    char firmwareUrl[512];
    char firmwareSha256[65];
    size_t firmwareSize;
    bool tagFound;
    bool firmwareFound;
  };

  // GitHub's GET /releases answers with an array of releases rather than one. The parser
  // reports them one at a time: the visitor runs as each release object closes, for those
  // with a tag and a matching firmware asset, newest first in GitHub's order.
  using ReleaseVisitor = void (*)(void* ctx, const Release& release);

  explicit ReleaseJsonParser(AssetMatcher assetMatcher = nullptr, ReleaseVisitor releaseVisitor = nullptr,
                             void* releaseVisitorCtx = nullptr);

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  // True once the response turned out to be an array of releases. The accessors below then
  // describe the last release in it; the visitor has seen each one.
  bool isReleaseList() const;

  const Release& release() const;
  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;
  const char* getFirmwareSha256() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
    ASSET_SHA256,
    ASSET_DIGEST,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();
  void commitRelease();
  void clearRelease();
  // Object depth at which a release's own keys sit: 1 in a single release, 2 in a list.
  uint8_t releaseDepth() const { return releaseList ? 2 : 1; }

  StreamingJsonParser parser;
  AssetMatcher assetMatcher;
  ReleaseVisitor releaseVisitor;
  void* releaseVisitorCtx;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;
  bool releaseList;

  Release current;

  char currentAssetName[96];
  char currentAssetUrl[512];
  char currentAssetSha256[65];
  size_t currentAssetSize;
};
