#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "AppVersion.h"
#include "FirmwareFlasher.h"
#include "FirmwareVersion.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "network/HttpDownloader.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
#ifndef CROSSINK_OTA_RELEASE_URL
#define CROSSINK_OTA_RELEASE_URL "https://api.github.com/repos/uxjulia/CrossInk/releases/latest"
#endif

// GitHub's /releases/latest never returns a pre-release, so a check that also wants release
// candidates reads the newest few releases instead and keeps the highest version. GitHub
// lists releases newest first, so the highest sits near the top; each one listed costs
// about 12 KB of response.
#ifndef CROSSINK_OTA_RELEASE_LIST_URL
#define CROSSINK_OTA_RELEASE_LIST_URL "https://api.github.com/repos/uxjulia/CrossInk/releases?per_page=5"
#endif

constexpr char latestReleaseUrl[] = CROSSINK_OTA_RELEASE_URL;
constexpr char releaseListUrl[] = CROSSINK_OTA_RELEASE_LIST_URL;

#ifdef CROSSINK_FIRMWARE_DEVICE_TYPE
constexpr char firmwareAssetStem[] = "firmware-" CROSSINK_FIRMWARE_DEVICE_TYPE;
constexpr char firmwareAssetName[] = "firmware-" CROSSINK_FIRMWARE_DEVICE_TYPE ".bin";
#else
constexpr char firmwareAssetStem[] = "firmware";
constexpr char firmwareAssetName[] = "firmware.bin";
#endif

constexpr char binSuffix[] = ".bin";
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr size_t OTA_HASH_CHUNK = 4096;
constexpr char OTA_STAGE_DIR[] = "/.crosspoint";
constexpr char OTA_STAGE_PATH[] = "/.crosspoint/ota-update.bin";

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

char lowerHex(const uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

char asciiLower(const char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isHexChar(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isSha256Hex(const char* value) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isHexChar(value[i])) return false;
  }
  return true;
}

bool sha256Matches(const uint8_t digest[32], const char* expectedHex) {
  if (!isSha256Hex(expectedHex)) return false;

  for (size_t i = 0; i < 32; ++i) {
    const char high = lowerHex((digest[i] >> 4) & 0x0F);
    const char low = lowerHex(digest[i] & 0x0F);
    if (high != asciiLower(expectedHex[i * 2]) || low != asciiLower(expectedHex[i * 2 + 1])) return false;
  }
  return true;
}

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0; }

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

size_t totalBytesReceived = 0;

struct OtaInstallContext {
  size_t* processedSize = nullptr;
  size_t totalSize = 0;
  size_t lastProgressBytes = 0;
  int lastReportedPct = -1;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* progressCtx = nullptr;
};

// Visits each release of a GET /releases list and keeps, in `ctx`, the newest one this
// device's firmware can be ordered against. On a tie the earlier, more recent entry stays.
void keepNewestListedRelease(void* ctx, const ReleaseJsonParser::Release& release) {
  auto* newest = static_cast<ReleaseJsonParser::Release*>(ctx);
  if (!firmware_version::isComparable(release.tagName)) {
    LOG_DBG("OTA", "Skipping listed release %s: its tag is not a version", release.tagName);
    return;
  }
  if (newest->tagFound && firmware_version::compare(release.tagName, newest->tagName) <= 0) return;
  *newest = release;
}

esp_err_t release_manifest_event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}

void notifyOtaProgress(OtaInstallContext* ctx, const bool force) {
  if (ctx == nullptr || ctx->onProgress == nullptr || ctx->processedSize == nullptr || ctx->totalSize == 0) return;

  const size_t processed = *ctx->processedSize;
  const int pct = static_cast<int>(static_cast<uint64_t>(processed) * 100 / ctx->totalSize);
  if (force || pct != ctx->lastReportedPct || processed - ctx->lastProgressBytes >= OTA_PROGRESS_UPDATE_BYTES) {
    ctx->lastReportedPct = pct;
    ctx->lastProgressBytes = processed;
    ctx->onProgress(ctx->progressCtx);
  }
}

enum class StagedHashResult {
  OK,
  OPEN_FAIL,
  OOM,
  READ_FAIL,
  MISMATCH,
};

// The OTA manifest digest covers the entire staged file, while
// validateImageFile() checks the ESP image's own integrity trailer. Keep both
// checks: the manifest pins the downloaded release before the flasher can
// erase the destination partition.
StagedHashResult verifyStagedHash(HalFile& file, const char* expectedHex, size_t* stagedSize) {
  if (!file.seek(0)) {
    LOG_ERR("OTA", "Failed to seek staged firmware before hashing");
    return StagedHashResult::READ_FAIL;
  }

  const size_t size = file.fileSize();
  if (stagedSize != nullptr) *stagedSize = size;

  // One 4 KiB buffer is required to hash the file without a whole-image
  // allocation; it is released before the firmware flasher allocates its own
  // chunk buffer.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(OTA_HASH_CHUNK);
  if (!buffer) {
    LOG_ERR("OTA", "OOM hashing staged firmware (%zu-byte buffer)", OTA_HASH_CHUNK);
    return StagedHashResult::OOM;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  size_t remaining = size;
  while (remaining > 0) {
    const size_t want = std::min(remaining, OTA_HASH_CHUNK);
    const int read = file.read(buffer.get(), want);
    if (read <= 0 || static_cast<size_t>(read) != want) {
      LOG_ERR("OTA", "Failed hashing staged firmware after %zu/%zu bytes", size - remaining, size);
      mbedtls_sha256_free(&shaCtx);
      return StagedHashResult::READ_FAIL;
    }
    mbedtls_sha256_update(&shaCtx, buffer.get(), want);
    remaining -= want;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);
  if (!file.seek(0)) {
    LOG_ERR("OTA", "Failed to rewind staged firmware after hashing");
    return StagedHashResult::READ_FAIL;
  }
  return sha256Matches(digest, expectedHex) ? StagedHashResult::OK : StagedHashResult::MISMATCH;
}

}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSha256.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  // The newest listed release lives on the heap, not next to the parser: an OTA launched at
  // boot checks inside setup()'s frames, on the 8 KB loop stack the TLS handshake also uses
  // (OtaUpdateActivity::onEnter). Value-initialised, so it starts with nothing found.
  std::unique_ptr<ReleaseJsonParser::Release> newestListed;
  if (includePrereleases) {
    newestListed = makeUniqueNoThrow<ReleaseJsonParser::Release>();
    if (!newestListed) {
      LOG_ERR("OTA", "OOM for the release list check (%zu bytes)", sizeof(ReleaseJsonParser::Release));
      return OOM_ERROR;
    }
  }

  esp_err_t esp_err;
  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName, newestListed ? keepNewestListedRelease : nullptr,
                                  newestListed.get());

  esp_http_client_config_t client_config = {
      .url = includePrereleases ? releaseListUrl : latestReleaseUrl,
      .event_handler = release_manifest_event_handler,
      // 4096 holds the API response headers; the 32KB body streams through the
      // parser in chunks so RX needn't be larger. TX only carries our GET.
      // Both free before installUpdate, so smaller leaves it less fragmentation.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .user_data = &releaseParser,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s, pre-releases: %s)", AppVersion::version(),
          includePrereleases ? "yes" : "no");

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", AppVersion::userAgent());
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);

  // A list leaves the parser on its last release; the one to offer is the newest kept above.
  // Any other answer to the list request (GitHub's rate-limit message, say) is an object and
  // fails below the way it does for /releases/latest.
  const ReleaseJsonParser::Release* release = &releaseParser.release();
  if (releaseParser.isReleaseList()) {
    if (!newestListed || !newestListed->tagFound) {
      LOG_ERR("OTA", "No listed release offers a %s asset", firmwareAssetStem);
      return NO_UPDATE;
    }
    release = newestListed.get();
  }
  LOG_DBG("OTA", "Parser results: list=%s tag=%s firmware=%s", releaseParser.isReleaseList() ? "yes" : "no",
          release->tagFound ? "yes" : "no", release->firmwareFound ? "yes" : "no");

  if (!release->tagFound) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = release->tagName;

  if (!release->firmwareFound) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = release->firmwareUrl;
  otaSha256 = release->firmwareSha256;
  otaSize = release->firmwareSize;
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu sha256=%s", latestVersion.c_str(), otaSize,
          otaSha256.empty() ? "missing" : "present");
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == AppVersion::version()) {
    return false;
  }

  const int comparison = firmware_version::compare(latestVersion.c_str(), AppVersion::version());
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latestVersion.c_str(), AppVersion::version(),
          comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }
  const bool hasManifestSha256 = isSha256Hex(otaSha256.c_str());
  if (!otaSha256.empty() && !hasManifestSha256) {
    LOG_ERR("OTA", "Refusing firmware with invalid manifest sha256");
    return JSON_PARSE_ERROR;
  }
  if (isHttpUrl(otaUrl) && !hasManifestSha256) {
    LOG_ERR("OTA", "Refusing HTTP firmware URL without manifest sha256");
    return JSON_PARSE_ERROR;
  }

  processedSize = 0;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "No OTA update partition found");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware too large: %zu > %zu", otaSize, updatePartition->size);
    return INTERNAL_UPDATE_ERROR;
  }
  // OTA reports download and flash as one unit of work, so the progress bar
  // advances monotonically instead of reaching 100% at staging and restarting.
  size_t stagingWork = otaSize;
  totalSize = stagingWork > 0 ? stagingWork * 2 : 0;
  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  WifiPowerSaveGuard wifiPowerSaveGuard;

  // Keep the image on SD until its chip metadata, integrity trailer, manifest
  // digest, and board tag have all passed validation. In particular, do not
  // erase or write the OTA partition while the tag may still be later in the
  // image's .rodata.
  if (!Storage.ensureDirectoryExists(OTA_STAGE_DIR)) {
    LOG_ERR("OTA", "Failed to create OTA staging directory: %s", OTA_STAGE_DIR);
    return INTERNAL_UPDATE_ERROR;
  }
  Storage.remove(OTA_STAGE_PATH);

  // Remove a previous partial download before measuring free space. Some SD
  // transports cannot report capacity; in that case let the download surface
  // its actual I/O failure instead of reporting a false card-full error.
  if (otaSize > 0) {
    const uint64_t totalBytes = Storage.totalBytes();
    const uint64_t usedBytes = Storage.usedBytes();
    const uint64_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
    if (totalBytes > 0 && freeBytes < otaSize) {
      LOG_ERR("OTA", "Insufficient SD space for OTA: free=%llu required=%zu",
              static_cast<unsigned long long>(freeBytes), otaSize);
      return SD_CARD_FULL_ERROR;
    }
  }

  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = isCancellationRequested;
  // wolfSSL currently has no CA bundle, so only use it when the trusted release
  // manifest supplied a digest that pins the firmware bytes. Older HTTPS
  // releases without a digest retain the verified esp_http_client path.
  if (hasManifestSha256) downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  LOG_INF("OTA", "Staging firmware download: url=%s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  const auto transferResult = HttpDownloader::downloadToFile(
      otaUrl, OTA_STAGE_PATH,
      [&](const size_t downloaded, const size_t total) {
        if (stagingWork == 0 && total > 0) {
          stagingWork = total;
          totalSize = stagingWork * 2;
          installCtx.totalSize = totalSize;
        }
        processedSize = downloaded;
        notifyOtaProgress(&installCtx, false);
      },
      nullptr, "", "", std::move(downloadOptions));

  if (transferResult != HttpDownloader::OK) {
    if (transferResult == HttpDownloader::ABORTED || isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      return CANCELLED_ERROR;
    }
    LOG_ERR("OTA", "Firmware download failed after %zu/%zu bytes", processedSize, totalSize);
    return HTTP_ERROR;
  }

  HalFile stagedFile;
  if (!Storage.openFileForRead("OTA", OTA_STAGE_PATH, stagedFile) || !stagedFile) {
    LOG_ERR("OTA", "Failed to open staged firmware: %s", OTA_STAGE_PATH);
    Storage.remove(OTA_STAGE_PATH);
    return INTERNAL_UPDATE_ERROR;
  }

  size_t stagedSize = stagedFile.fileSize();
  if (hasManifestSha256) {
    const StagedHashResult hashResult = verifyStagedHash(stagedFile, otaSha256.c_str(), &stagedSize);
    if (hashResult == StagedHashResult::MISMATCH) {
      LOG_ERR("OTA", "Firmware sha256 mismatch: expected=%s", otaSha256.c_str());
      stagedFile.close();
      Storage.remove(OTA_STAGE_PATH);
      return HASH_MISMATCH_ERROR;
    }
    if (hashResult != StagedHashResult::OK) {
      stagedFile.close();
      Storage.remove(OTA_STAGE_PATH);
      return hashResult == StagedHashResult::OOM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
    }
    LOG_INF("OTA", "Firmware sha256 verified");
  }

  if (otaSize > 0 && stagedSize != otaSize) {
    LOG_ERR("OTA", "Firmware size mismatch: got %zu, expected %zu", stagedSize, otaSize);
    stagedFile.close();
    Storage.remove(OTA_STAGE_PATH);
    return INTERNAL_UPDATE_ERROR;
  }

  const auto validationResult = firmware_flash::validateOpenImageFile(stagedFile, updatePartition->size);
  if (validationResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Staged firmware validation failed: %s", firmware_flash::resultName(validationResult));
    stagedFile.close();
    Storage.remove(OTA_STAGE_PATH);
    if (validationResult == firmware_flash::Result::BAD_CHIP ||
        validationResult == firmware_flash::Result::WRONG_BOARD) {
      return WRONG_DEVICE_ERROR;
    }
    return validationResult == firmware_flash::Result::OOM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
  }

  // This call is the first operation that can erase/write the OTA partition.
  // The file passed all checks immediately above, including the board tag.
  if (stagingWork == 0) stagingWork = stagedSize;
  processedSize = stagingWork;
  totalSize = stagingWork + stagedSize;
  installCtx.totalSize = totalSize;
  const auto flashResult = firmware_flash::flashValidatedFile(
      stagedFile,
      [](const size_t written, const size_t total, void* context) {
        auto* install = static_cast<OtaInstallContext*>(context);
        const size_t stagingBytes = install->totalSize > total ? install->totalSize - total : 0;
        *install->processedSize = stagingBytes + written;
        notifyOtaProgress(install, false);
      },
      &installCtx);
  stagedFile.close();
  Storage.remove(OTA_STAGE_PATH);
  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Firmware flash failed: %s", firmware_flash::resultName(flashResult));
    return flashResult == firmware_flash::Result::OOM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
  }

  notifyOtaProgress(&installCtx, true);
  LOG_INF("OTA", "Update completed: %zu bytes", processedSize);
  return OK;
}
#endif
