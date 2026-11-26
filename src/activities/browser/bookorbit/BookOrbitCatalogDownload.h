#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "network/BookOrbitCatalogClient.h"
#include "network/HttpDownloader.h"

/**
 * A BookOrbit catalog book's file on the SD card: finding a copy already there, where a
 * download goes, and the transfer itself.
 */
namespace BookOrbitCatalogDownload {

/**
 * What the transfer tells the download screen, and the input it checks. Implemented by
 * BookOrbitCatalogBrowserActivity, which owns the screen state and its RenderLock.
 */
class Reporter {
 public:
  // Called once the transfer is set up, right before its first request: whatever the
  // caller can free goes now, so its block joins the heap the TLS session starts from.
  virtual void releaseForTransfer() = 0;
  // Every chunk received. redraw: the screen should repaint, throttled to meaningful
  // steps (partial refreshes still wear the panel).
  virtual void reportProgress(size_t downloaded, size_t total, bool redraw) = 0;
  // Polls the input: true when the user asked to stop. Asked until it says so once.
  virtual bool cancelRequested() = 0;

 protected:
  ~Reporter() = default;
};

/**
 * True when the catalog book already exists locally, with its path in outPath. The
 * download index knows where past downloads landed, whatever the folder setting or the
 * server's naming template was at the time; the fallback filename heuristic covers
 * pre-index downloads in the current download location, the SD root (the old fixed
 * location) and the /Read folder the finished-book move feature uses. It cannot cover the
 * server's naming template: only the book detail carries the resolved path, and a listing
 * would need one request per row to know it. Books downloaded under a template but missing
 * from the index (another device, or an index dropped when the server URL changed) are
 * therefore not detected — as with books moved or renamed by hand, this stays a
 * best-effort convenience marker.
 */
bool findOnDevice(int64_t bookId, const std::string& title, const std::string& author, std::string& outPath);

// findOnDevice() for a page of books, with one pass over the download index for all of them:
// outPaths[i] is where books[i] is, or empty. outPaths holds books.size() strings.
void findOnDevice(const std::vector<BookOrbitCatalogBook>& books, std::string* outPaths);

// The book's EPUB file among its formats; false when it has none.
bool findEpubFile(const BookOrbitBookDetail& detail, BookOrbitCatalogFile& outFile);

/**
 * The full SD path a catalog book downloads to: the configured download folder ("" = SD
 * root), then where the file belongs under it. BookOrbit resolves the account's KOReader
 * file naming template (or this device's override) and returns it as the file's
 * devicePath, so a book lands where the same account's KOReader devices put it — series
 * folders and all. Servers that do not send one keep the title-author name this fork
 * always used.
 */
std::string destination(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file);

/**
 * A book another app sent or a copy over USB put at the very path the server's naming
 * template gives it is not in the download index, so its row was not marked. The details
 * name that path and the library file's size: a file there of that size is this book.
 */
bool isAlreadyAt(const std::string& path, const BookOrbitCatalogFile& file);

// Creates the download folder and, when the server's naming template nests the book
// ("Sprawl/<book>.epub"), every folder it asks for. False, logged, when the card refuses.
bool makeFolders(const std::string& path);

/**
 * Downloads the file to finalPath, staged in the private transfer directory and only put in
 * place once it opens as an EPUB. variant is the file's downloadVariant: with it, an attempt
 * the connection drops resumes from the staged bytes, and the download only fails after three
 * attempts in a row that end with no more of the file than it ever had. A copy it replaces
 * stays beside the new one until BookUpdateMigration has moved the state keyed by its content.
 * On OK, that hand-over is done (or the book's render cache cleared), and the download index
 * and the Library know of the book. Giving up keeps the bytes received, with their version,
 * for the next download of this book to resume (/.crosspoint/bookorbit_partial.json); one
 * book's at most, as starting another drops them. A cancel or a full card removes them.
 */
HttpDownloader::DownloadError transfer(int64_t bookId, int64_t fileId, const std::string& variant,
                                       const std::string& finalPath, Reporter& reporter);

}  // namespace BookOrbitCatalogDownload
