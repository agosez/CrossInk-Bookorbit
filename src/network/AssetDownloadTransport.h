#pragma once

#include "HttpDownloader.h"
#if !defined(SIMULATOR) && __has_include(<BoardConfig.h>)
#include <BoardConfig.h>
#endif
#if defined(ARDUINO) && !defined(SIMULATOR) && !defined(FREEINK_MCU_C3)
#error "AssetDownloadService::transport() needs BoardConfig.h on device builds"
#endif

// Kept out of AssetDownloadService.h, which stays free of the HAL so the build identity
// checks (scripts/test_build_identity.py) can compile it on the host.
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
}  // namespace AssetDownloadService
