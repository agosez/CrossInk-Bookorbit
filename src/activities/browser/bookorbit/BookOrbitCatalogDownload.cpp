#include "BookOrbitCatalogDownload.h"

#include <Arduino.h>
#include <FilePublication.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include "AppVersion.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"
#include "util/BookCacheUtils.h"
#include "util/BookOrbitDevicePath.h"
#include "util/BookUpdateMigration.h"

namespace BookOrbitCatalogDownload {
namespace {
constexpr size_t BOOKORBIT_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr char READ_FOLDER_PREFIX[] = "/Read";
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 1000;
constexpr unsigned long DOWNLOAD_PROGRESS_MAX_UPDATE_MS = 5000;
constexpr int MAX_DOWNLOAD_ATTEMPTS = 3;
// HttpDownloader's FilePublication staging kind: a download waits below
// /.crosspoint/xfer/<kind>/ until it is complete and checked.
constexpr char STAGED_DOWNLOAD_KIND[] = "http-download";

// The SD filename (leading slash, no folder) this firmware falls back to when the
// server does not say where the file belongs.
std::string catalogBookFilename(const std::string& title, const std::string& author) {
  return BookOrbitDevicePath::catalogFilename("", title, author);
}

// The filename heuristic behind the download index (see findOnDevice()).
bool findByFilename(const std::string& title, const std::string& author, std::string& outPath) {
  const std::string filename = catalogBookFilename(title, author);
  const std::string& folder = BOOKORBIT_STORE.getDownloadFolder();
  // Candidates are built aside: outPath is a listed row's own path, and most catalog
  // books are not on the device, so a miss must not leave a path buffer in the row.
  const auto found = [&outPath](std::string candidate) {
    if (!Storage.exists(candidate.c_str())) return false;
    outPath = std::move(candidate);
    return true;
  };
  if (!folder.empty() && found(folder + filename)) return true;
  if (found(filename) || found(READ_FOLDER_PREFIX + filename)) return true;
  std::string().swap(outPath);  // re-checked after a delete: release the old path too
  return false;
}
}  // namespace

bool findOnDevice(const int64_t bookId, const std::string& title, const std::string& author, std::string& outPath) {
  return BookOrbitDownloadIndex::lookup(bookId, outPath) || findByFilename(title, author, outPath);
}

void findOnDevice(const std::vector<BookOrbitCatalogBook>& books, std::string* outPaths) {
  const size_t count = books.size();
  if (count == 0 || outPaths == nullptr) return;
  // A page of ids: runtime-sized, so neither stack nor static storage fits.
  auto ids = makeUniqueNoThrow<int64_t[]>(count);
  if (ids) {
    for (size_t i = 0; i < count; i++) ids[i] = books[i].id;
    BookOrbitDownloadIndex::lookupAll(
        ids.get(), count,
        [](const size_t i, const std::string& path, void* context) { static_cast<std::string*>(context)[i] = path; },
        outPaths);
  } else {
    LOG_ERR("BookOrbit", "Out of memory marking %u books on the device", static_cast<unsigned>(count));
  }
  for (size_t i = 0; i < count; i++) {
    if (outPaths[i].empty()) findByFilename(books[i].title, books[i].author, outPaths[i]);
  }
}

bool findEpubFile(const BookOrbitBookDetail& detail, BookOrbitCatalogFile& outFile) {
  for (const auto& file : detail.files) {
    std::string format = file.format;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return std::tolower(c); });
    if (format == "epub") {
      outFile = file;
      return true;
    }
  }
  return false;
}

std::string destination(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file) {
  return BOOKORBIT_STORE.getDownloadFolder() +
         BookOrbitDevicePath::catalogFilename(file.devicePath, detail.title, detail.author);
}

bool isAlreadyAt(const std::string& path, const BookOrbitCatalogFile& file) {
  if (file.sizeBytes == 0) return false;
  FsFile existing = Storage.open(path.c_str());  // quiet when there is none, the usual case
  const bool same = existing && !existing.isDirectory() && existing.fileSize64() == file.sizeBytes;
  existing.close();
  return same;
}

bool makeFolders(const std::string& path) {
  // mkdir's pFlag creates the missing parents in one call, so this covers both at once.
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == 0 || lastSlash == std::string::npos) return true;
  const std::string folder = path.substr(0, lastSlash);
  if (Storage.exists(folder.c_str()) || Storage.mkdir(folder.c_str())) return true;
  LOG_ERR("BookOrbit", "Could not create download folder %s", folder.c_str());
  return false;
}

HttpDownloader::DownloadError transfer(const int64_t bookId, const int64_t fileId, const std::string& finalPath,
                                       Reporter& reporter) {
  // The heap the TLS session starts from: what tells a fragmented session from a
  // clean download boot in a field log. The version says which build wrote that log.
  LOG_INF("BookOrbit", "Downloading file %lld -> %s (free=%u maxAlloc=%u, firmware %s)", static_cast<long long>(fileId),
          finalPath.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), AppVersion::version());

  bool cancelRequested = false;
  auto pollCancel = [&reporter, &cancelRequested] {
    if (cancelRequested) return true;
    cancelRequested = reporter.cancelRequested();
    return cancelRequested;
  };

  // A replacement a reset interrupted: this download would drop its old copy
  // (DownloadFileSwap::recover, keepReplaced below), so the state that copy holds moves first.
  if (BookUpdateMigration::pending(finalPath)) BookUpdateMigration::begin(finalPath, /*renderCacheIsOld=*/false);

  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = pollCancel;
  downloadOptions.bufferSize = BOOKORBIT_DOWNLOAD_BUFFER_SIZE;
  // TLS runs with a few KB of headroom on the C3 and reads can drop mid-transfer;
  // keep the partial file and resume instead of restarting a multi-MB download.
  downloadOptions.preservePartial = true;
  // A book spans many TLS records: once the request is out, the SDK grows the
  // connection's input buffer to a full record while the heap still has the block
  // (skipped when the server agreed to small records, issue #96).
  downloadOptions.reserveFullRecord = true;
  // CrossInk's download safeguards, as its OPDS browser uses them: stage the file in the
  // private transfer directory, refuse a book the card cannot hold before writing any of it, and
  // only put the file in place once it opens as an EPUB. A body cut short without a
  // Content-Length, or an error page served as the file, used to land as the book.
  downloadOptions.stageAsPart = true;
  // A re-download's highlights, bookmarks and sync state are keyed by the old copy's content;
  // it stays beside the new one until they have moved (see the OK branch below).
  downloadOptions.keepReplaced = true;
  downloadOptions.checkFreeSpace = true;
  downloadOptions.validate = [](const std::string& path) {
    ZipFile zip(path);
    size_t size = 0;
    return zip.getInflatedFileSize("META-INF/container.xml", &size) && size > 0;
  };

  reporter.releaseForTransfer();

  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  unsigned long now = 0;
  int percent = 0;
  for (int attempt = 0; attempt < MAX_DOWNLOAD_ATTEMPTS; attempt++) {
    // Honor a Back press between attempts too: a failing download otherwise runs
    // all its retries (TLS handshake included) with the cancel request ignored.
    if (pollCancel()) {
      result = HttpDownloader::ABORTED;
      break;
    }
    downloadOptions.resumePartial = attempt > 0;
    result = BookOrbitCatalogClient::downloadFile(
        fileId, finalPath,
        [&reporter, &lastRenderedPercent, &lastProgressUpdateMs, &percent, &now](const size_t downloaded,
                                                                                 const size_t total) {
          percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
          now = millis();
          // Throttle e-ink refreshes to meaningful progress steps: partial
          // refreshes still wear the panel, and a raw byte-count callback fires
          // far more often than the screen needs to redraw.
          const bool redraw = percent >= 100 || lastRenderedPercent < 0 ||
                              (percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT &&
                               now > DOWNLOAD_PROGRESS_MIN_UPDATE_MS) ||
                              now - lastProgressUpdateMs > DOWNLOAD_PROGRESS_MAX_UPDATE_MS;
          if (redraw) {
            lastRenderedPercent = percent;
            lastProgressUpdateMs = now;
          }
          reporter.reportProgress(downloaded, total, redraw);
        },
        &cancelRequested, downloadOptions);
    // A full card will not have more room on the next attempt.
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED ||
        result == HttpDownloader::INSUFFICIENT_SPACE) {
      break;
    }
    LOG_ERR("BookOrbit", "Download attempt %d/%d failed (err=%d), retrying", attempt + 1, MAX_DOWNLOAD_ATTEMPTS,
            static_cast<int>(result));
  }

  if (result != HttpDownloader::OK && result != HttpDownloader::ABORTED) {
    // preservePartial kept the staged file for resuming between attempts; don't
    // leave it behind once we give up.
    std::string staged;
    if (FilePublication::stagePath(finalPath, STAGED_DOWNLOAD_KIND, staged)) Storage.remove(staged.c_str());
  }

  if (result == HttpDownloader::OK) {
    // A copy it replaced hands its state to the new one; that also clears the render cache.
    if (BookUpdateMigration::pending(finalPath)) {
      BookUpdateMigration::begin(finalPath, /*renderCacheIsOld=*/true);
    } else {
      clearBookCache(finalPath);
    }
    BookOrbitDownloadIndex::record(bookId, finalPath);
    // A new book, or new contents under a known path: the Library's index is stale.
    library::invalidateLibraryIndex();
  }
  return result;
}

}  // namespace BookOrbitCatalogDownload
