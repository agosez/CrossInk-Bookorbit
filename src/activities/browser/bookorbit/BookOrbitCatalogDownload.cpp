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
#include "BookOrbitCatalogListCache.h"
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
// Attempts in a row that may end without the download getting further before it fails.
constexpr uint8_t MAX_ATTEMPTS_WITHOUT_PROGRESS = 3;
// The pause before an attempt that follows one without progress, times the attempts in a
// row so far: a Wi-Fi drop of a few seconds is ridden out rather than failed in a burst.
constexpr unsigned long STALLED_RETRY_DELAY_MS = 1000;
// HttpDownloader's FilePublication staging kind: a download waits below
// /.crosspoint/xfer/<kind>/ until it is complete and checked.
constexpr char STAGED_DOWNLOAD_KIND[] = "http-download";

// Counts the attempts that end without the download getting further than it ever got. The
// furthest point, not the previous attempt's: a server that drops the connection and then
// ignores the resume restarts every other attempt from zero, which must not count as progress
// for ever.
class ProgressBudget {
 public:
  // resumedBytes: what an earlier download left for this one; only more counts as progress.
  explicit ProgressBudget(const size_t resumedBytes) : furthest_(resumedBytes) {}
  // keptBytes: what the staged file holds after a failed attempt. False once it should stop.
  bool again(const size_t keptBytes) {
    if (keptBytes > furthest_) {
      furthest_ = keptBytes;
      withoutProgress_ = 0;
    } else {
      ++withoutProgress_;
    }
    return withoutProgress_ < MAX_ATTEMPTS_WITHOUT_PROGRESS;
  }
  uint8_t withoutProgress() const { return withoutProgress_; }

 private:
  size_t furthest_ = 0;
  uint8_t withoutProgress_ = 0;
};

// What a failed attempt left in the staged file for the next one to resume (0 when none).
size_t stagedBytes(const std::string& stagedPath) {
  FsFile staged = Storage.open(stagedPath.c_str());  // quiet when the attempt removed it
  const size_t size = staged && !staged.isDirectory() ? staged.fileSize() : 0;
  staged.close();
  return size;
}

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

HttpDownloader::DownloadError transfer(const int64_t bookId, const int64_t fileId, const std::string& variant,
                                       const std::string& finalPath, Reporter& reporter) {
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
  // One request per attempt: the loop below owns the retries, so every attempt counts toward
  // its rule, whether the connection dropped or the server answered with an error.
  downloadOptions.maxAttempts = 1;
  // The validator (the server's ETag) the staged bytes came under. The next attempt resumes
  // them with Range and If-Range, so a book updated on the server in between starts over
  // instead of splicing two versions. Empty: nothing to resume, the next attempt starts over.
  std::string validator;
  downloadOptions.resumeValidator = &validator;
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

  std::string stagedPath;
  if (!FilePublication::stagePath(finalPath, STAGED_DOWNLOAD_KIND, stagedPath)) stagedPath.clear();
  // Cleared once the server's delivery route refuses the file (400 when busy, 409 when it
  // changed on disk before a rescan): the plain route still serves it, only without resuming.
  bool useVariant = !variant.empty();
  const std::string noVariant;
  size_t keptBytes = 0;

  // The bytes an earlier download left (BookOrbitKeptDownload, written before the first byte
  // of each): resumed when they are this book's, in the version it asks for, and dropped when
  // they are another book's, so the card keeps one at most. Its validator, once set, is the
  // version of every staged byte: a resume only appends bytes the server confirmed under it,
  // and an attempt that starts over clears it first.
  BookOrbitKeptDownload onCard;
  const bool hasRecord = BookOrbitCatalogListCache::loadKeptDownload(onCard);
  if (hasRecord && onCard.path != finalPath) {
    std::string otherStaged;
    if (FilePublication::stagePath(onCard.path, STAGED_DOWNLOAD_KIND, otherStaged)) Storage.remove(otherStaged.c_str());
    LOG_INF("BookOrbit", "Dropped the bytes an unfinished download of %s kept", onCard.path.c_str());
  } else if (hasRecord && onCard.fileId == fileId && onCard.variant == variant && !onCard.validator.empty() &&
             !stagedPath.empty()) {
    keptBytes = stagedBytes(stagedPath);
    if (keptBytes > 0) {
      validator = onCard.validator;
      LOG_INF("BookOrbit", "Resuming the %zu bytes an earlier download of this book kept", keptBytes);
    }
  }
  ProgressBudget budget(keptBytes);
  // What the card says about the staged bytes, rewritten only when that changes.
  bool recorded = hasRecord && keptBytes > 0;
  std::string recordedValidator = recorded ? validator : std::string();
  const auto recordStaged = [&](const std::string& forValidator) {
    if (recorded && recordedValidator == forValidator) return;
    recorded = BookOrbitCatalogListCache::saveKeptDownload(
        {fileId, finalPath, useVariant ? variant : noVariant, forValidator});
    recordedValidator = forValidator;
  };

  reporter.releaseForTransfer();

  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  bool gaveUp = false;  // ended by the attempts in a row without progress
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  unsigned long now = 0;
  int percent = 0;
  for (unsigned attempt = 1;; attempt++) {
    // Honor a Back press between attempts too: a failing download otherwise runs
    // all its retries (TLS handshake included) with the cancel request ignored.
    if (pollCancel()) {
      result = HttpDownloader::ABORTED;
      break;
    }
    // Without a validator nothing says which version staged bytes belong to: the attempt
    // drops them and starts over, and the card stops vouching for any version first.
    downloadOptions.resumePartial = keptBytes > 0 && !validator.empty();
    if (!downloadOptions.resumePartial) recordStaged(std::string());
    result = BookOrbitCatalogClient::downloadFile(
        fileId, useVariant ? variant : noVariant, finalPath,
        [&reporter, &lastRenderedPercent, &lastProgressUpdateMs, &percent, &now, &validator, &recordStaged](
            const size_t downloaded, const size_t total) {
          // The first bytes of a new answer: once the card names their version, a reset or a
          // flat battery from here on leaves them for the next download of this book.
          if (!validator.empty()) recordStaged(validator);
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
    const int status = HttpDownloader::lastHttpStatus;
    keptBytes = stagedPath.empty() ? 0 : stagedBytes(stagedPath);
    const bool retry = budget.again(keptBytes);
    LOG_ERR("BookOrbit", "Download attempt %u failed (err=%d, http=%d, %zu bytes kept, %u in a row without progress)%s",
            attempt, static_cast<int>(result), status, keptBytes, static_cast<unsigned>(budget.withoutProgress()),
            retry ? ", retrying" : "");
    if (keptBytes > 0 && !validator.empty()) recordStaged(validator);
    if (!retry) {
      gaveUp = true;
      break;
    }
    // The server answered the resume itself, yet the staged bytes got no further (416, or a
    // 206 for another range or version): they cannot be resumed, the next attempt starts over.
    if ((status == 206 || status == 416) && budget.withoutProgress() > 0) validator.clear();
    if (useVariant && (status == 400 || status == 409)) {
      useVariant = false;
      validator.clear();
    }
    const unsigned long pauseMs = STALLED_RETRY_DELAY_MS * budget.withoutProgress();
    for (const unsigned long start = millis(); millis() - start < pauseMs && !pollCancel();) delay(20);
  }

  // Giving up keeps what was received under a known version for the next download of this
  // book. Otherwise the staged file goes (a cancel, or a full card, included), and its record.
  const bool keep = gaveUp && keptBytes > 0 && !validator.empty() && recorded && recordedValidator == validator;
  if (keep) {
    LOG_INF("BookOrbit", "Keeping the %zu bytes received for a later download of this book", keptBytes);
  } else {
    if (result != HttpDownloader::OK && !stagedPath.empty()) Storage.remove(stagedPath.c_str());
    BookOrbitCatalogListCache::discardKeptDownload();
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
