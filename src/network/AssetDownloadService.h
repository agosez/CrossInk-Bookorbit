#pragma once

#include <cstddef>
#include <string_view>

#include "AssetCatalog.h"
#include "HttpDownloader.h"
#if !defined(SIMULATOR) && __has_include(<BoardConfig.h>)
#include <BoardConfig.h>
#endif
#if defined(ARDUINO) && !defined(SIMULATOR) && !defined(FREEINK_MCU_C3)
#error "AssetDownloadService::transport() needs BoardConfig.h on device builds"
#endif

// Catalog selection and SD sources. Activation remains an explicit installer action.
namespace AssetDownloadService {
// With Wi-Fi up, the C3 has about 47 KB left at this point, too little for an mbedTLS
// handshake with GitHub Pages or Cloudflare (MBEDTLS_ERR_SSL/X509_ALLOC_FAILED, or a stall
// until the timeout). wolfSSL fits, as for the BookOrbit and font downloads, but does not
// check the server's certificate: what authenticates these downloads there is the catalog's
// signature (AssetCatalogSignature). Boards with the heap keep the verified transport too.
constexpr HttpDownloader::Transport transport() {
#if !defined(SIMULATOR) && defined(FREEINK_MCU_C3) && FREEINK_MCU_C3
  return HttpDownloader::Transport::WOLFSSL;
#else
  return HttpDownloader::Transport::ESP_HTTP;
#endif
}

// Local builds may select a catalog channel without changing their version label.
constexpr AssetCatalog::Channel channelForVersion([[maybe_unused]] std::string_view version) {
#ifdef ASSET_DOWNLOAD_CHANNEL
  constexpr std::string_view channel = ASSET_DOWNLOAD_CHANNEL;
  static_assert(channel == "rc" || channel == "stable", "ASSET_DOWNLOAD_CHANNEL must be \"rc\" or \"stable\"");
  if constexpr (channel == "rc") {
    return AssetCatalog::Channel::Rc;
  } else {
    return AssetCatalog::Channel::Stable;
  }
#else
  return version.size() >= 3 && version.substr(version.size() - 3) == "-RC" ? AssetCatalog::Channel::Rc
                                                                            : AssetCatalog::Channel::Stable;
#endif
}

constexpr size_t MAX_PATH = 160;
constexpr size_t MAX_URL = 384;
enum class Result { Ok, Io, Memory, Cancelled, Invalid, Collision, OverwriteRequired, Unavailable, Network };
enum class Status { Missing, Current, Previous, Different, FileOnSd, Collision };
struct Source {
  Status status = Status::Missing;
  char path[MAX_PATH] = {};
  char sha256[65] = {};
  // A transaction sidecar remains; explicit Download/Retry performs recovery.
  bool recoveryPending = false;
};
struct Callbacks {
  void* context = nullptr;
  bool (*cancelled)(void*) = nullptr;
  void (*progress)(void*, size_t, size_t) = nullptr;
  void (*verifying)(void*) = nullptr;
  // Companions are verified on SD; the installable payload transfer starts next.
  void (*payloadStarted)(void*) = nullptr;
};
// Firmware-owned approved prefixes, ending in '/'. Empty defaults fail closed.
// No URL or redirect prefix comes from the catalog or user input.
struct Endpoints {
  const char* base = "";
  const char* redirectBase = "";
};
bool allowUrl(std::string_view url, void* endpoints);
bool buildUrl(const Endpoints& endpoints, const char* relative, char* out, size_t capacity);
// Offline entries with an empty sha256 classify existing published sources as FileOnSd.
Result discover(AssetCatalog::Kind kind, const AssetCatalog::Entry& entry, Source& source,
                const Callbacks& callbacks = {});
Result download(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry, const Endpoints& endpoints,
                bool overwriteApproved, const Callbacks& callbacks = {});
const char* resultName(Result result);
}  // namespace AssetDownloadService
