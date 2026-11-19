#include "BookOrbitLocalBooks.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibrarySort.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"

namespace {

constexpr char READ_FOLDER[] = "/Read";
constexpr char PATHS_FILE[] = "/.crosspoint/bookorbit_local.paths";
constexpr char ORDER_FILE[] = "/.crosspoint/bookorbit_local.order";
constexpr char RUN_FILE[] = "/.crosspoint/bookorbit_local.sort";
// Rows are numbered by the sorter's uint16_t ordinals.
constexpr int MAX_ROWS = INT16_MAX;
// One path per record, NUL-padded, so row k's path sits at k * PATH_BYTES, two records to a
// sector. The download index's own path limit: a path that fills its record has no NUL.
constexpr size_t PATH_BYTES = 256;
// A natural-sort key grows by two bytes per run of digits (see NaturalSort.cpp).
constexpr size_t KEY_BYTES = 2 * PATH_BYTES;

// The working memory of a walk or a sort, about 1 KB: too much for the catalog loop's
// stack, and needed only while the list is built, so it is allocated for that.
struct Scratch {
  char name[PATH_BYTES + 1];    // a folder entry's name
  char record[PATH_BYTES + 1];  // a path record, as written or read back, NUL-terminated
  uint8_t key[KEY_BYTES];
};

// Receives each book's path; false stops the walk.
using Visit = bool (*)(const std::string& path, void* context);

std::string_view parentOf(const std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return {};
  return path.substr(0, std::max<size_t>(slash, 1));  // "/" for a book at the root
}

// Visits the EPUBs directly in one folder. A folder that does not exist holds none.
bool walkFolder(const std::string& folder, Scratch& scratch, const Visit visit, void* context) {
  FsFile dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) {
    const bool missing = !dir.allocationFailed();
    dir.close();
    if (!missing) LOG_ERR("BookOrbit", "Could not open %s", folder.c_str());
    return missing;
  }
  const std::string prefix = folder == "/" ? folder : folder + "/";
  std::string path;  // reused: one allocation for the whole folder, not one per book
  path.reserve(PATH_BYTES);
  bool ok = true;
  FsFile file;
  while (ok && (file = dir.openNextFile())) {
    const size_t length = file.isDirectory() ? 0 : file.getName(scratch.name, sizeof(scratch.name));
    file.close();
    if (length <= 5 || !FsHelpers::hasEpubExtension(std::string_view(scratch.name, length))) continue;
    if (prefix.size() + length > PATH_BYTES) {
      LOG_ERR("BookOrbit", "Not listing %s%s: path too long", prefix.c_str(), scratch.name);
      continue;
    }
    path.assign(prefix).append(scratch.name, length);
    ok = visit(path, context);
  }
  const bool complete = ok && !FsHelpers::directoryIterationFailed(dir);
  dir.close();
  if (ok && !complete) LOG_ERR("BookOrbit", "Listing %s failed before its end", folder.c_str());
  return complete;
}

// Visits every book the list holds, once: the catalog downloads the index knows outside the
// three folders, wherever the naming template filed them, then the EPUBs directly in the
// download folder, the SD root and /Read. An indexed book in one of those folders comes from
// the folder, which also lists it once its file changed and the index no longer vouches for it.
bool walk(Scratch& scratch, const Visit visit, void* context) {
  const std::string& folder = BOOKORBIT_STORE.getDownloadFolder();  // "" for the SD root
  struct Indexed {
    const std::string& folder;
    Visit visit;
    void* context;
    bool ok;
  } indexed{folder, visit, context, true};
  BookOrbitDownloadIndex::forEachExisting(
      [](int64_t, const std::string& path, void* data) {
        auto& in = *static_cast<Indexed*>(data);
        const std::string_view parent = parentOf(path);
        if (!in.ok || parent == "/" || parent == READ_FOLDER || parent == in.folder) return;
        if (FsHelpers::hasEpubExtension(path)) in.ok = in.visit(path, in.context);
      },
      &indexed);
  if (!indexed.ok) return false;
  if (!folder.empty() && folder != READ_FOLDER && !walkFolder(folder, scratch, visit, context)) return false;
  return walkFolder("/", scratch, visit, context) && walkFolder(READ_FOLDER, scratch, visit, context);
}

// Reads row `row`'s path into `record` (PATH_BYTES + 1 bytes), NUL-terminated.
bool readRecord(FsFile& file, const uint16_t row, char* record) {
  const bool ok = file.seekSet(static_cast<size_t>(row) * PATH_BYTES) &&
                  file.read(record, PATH_BYTES) == static_cast<int>(PATH_BYTES);
  record[PATH_BYTES] = '\0';
  return ok && record[0] == '/';
}

// The row's sort key in scratch.key, and its length: the natural-sort key of the file name
// without ".epub", the title the catalog shows for the row. Cuts scratch.record down to that
// name.
size_t keyOf(Scratch& scratch) {
  char* name = strrchr(scratch.record, '/') + 1;
  const size_t length = strlen(name);
  if (length > 5) name[length - 5] = '\0';
  return FsHelpers::naturalSortKey(name, scratch.key, sizeof(scratch.key));
}

// Sorts the `rows` records of `paths` by name and writes their row numbers, in that order, to
// ORDER_FILE. Its own function so the sorter, which holds its run file open, is gone before
// a failed build removes that file.
bool sortRecords(FsFile& paths, const uint16_t rows, Scratch& scratch) {
  struct Source {
    FsFile& paths;
    Scratch& scratch;
  } source{paths, scratch};
  library::SortConfig config;
  config.runPath = RUN_FILE;
  config.keyBytes = KEY_BYTES;
  config.loadContext = &source;
  // Keys are compared 12 bytes at a time; only ties read the rest of a key from the card.
  config.load = [](void* context, const uint16_t row, const size_t offset, char* segment, size_t& length) {
    auto& in = *static_cast<Source*>(context);
    if (!readRecord(in.paths, row, in.scratch.record)) return false;
    length = keyOf(in.scratch);
    memset(segment, 0, library::SORT_SEGMENT_BYTES);
    if (offset < length) {
      memcpy(segment, in.scratch.key + offset, std::min(length - offset, library::SORT_SEGMENT_BYTES));
    }
    return true;
  };

  // Without PSRAM the buffer comes out of the heap the catalog's TLS sessions need: hold it
  // to the smallest the sorter retries with (4 KB), and let a longer list spill into runs
  // on the card.
  const uint16_t buffered = psramHeapAvailable() ? rows : std::min(rows, library::SORT_MIN_RUN_ENTRIES);
  library::ExternalSorter sorter;
  bool ok = sorter.begin(config, buffered);
  for (uint16_t row = 0; ok && row < rows; row++) {
    ok = readRecord(paths, row, scratch.record);
    if (!ok) break;
    const size_t length = keyOf(scratch);
    library::SortEntry entry{};
    memcpy(entry.key, scratch.key, std::min(length, sizeof(entry.key)));
    entry.ordinal = row;
    entry.source = row;
    ok = sorter.add(entry);
  }
  FsFile order;
  ok = ok && Storage.openFileForWrite("BookOrbit", ORDER_FILE, order);
  if (!ok) return false;
  ok = sorter.finish(
      [](void* context, const library::SortEntry& entry) {
        return serialization::tryWritePod(*static_cast<FsFile*>(context), entry.ordinal);
      },
      &order);
  return order.close() && ok;
}

}  // namespace

size_t BookOrbitLocalBooks::count() {
  auto scratch = makeUniqueNoThrow<Scratch>();
  if (!scratch) {
    LOG_ERR("BookOrbit", "Out of memory to count the books on the card");
    return 0;
  }
  size_t found = 0;
  const Visit countBook = [](const std::string&, void* context) {
    ++*static_cast<size_t*>(context);
    return true;
  };
  if (!walk(*scratch, countBook, &found)) LOG_ERR("BookOrbit", "Could not count every book on the card");
  LOG_DBG("BookOrbit", "Local scan: kind=on-device count=%u", static_cast<unsigned>(found));
  return found;
}

bool BookOrbitLocalBooks::build(int& total) {
  clear();
  total = 0;
  auto scratch = makeUniqueNoThrow<Scratch>();
  if (!scratch) {
    LOG_ERR("BookOrbit", "Out of memory to list the books on the card");
    return false;
  }
  FsFile paths;
  if (!Storage.openFileForWrite("BookOrbit", PATHS_FILE, paths)) {
    LOG_ERR("BookOrbit", "Could not create %s", PATHS_FILE);
    return false;
  }

  // One record per book, in the order the walk finds them.
  struct Staging {
    FsFile& paths;
    char* record;
    int rows;
  } staging{paths, scratch->record, 0};
  const Visit stage = [](const std::string& path, void* context) {
    auto& out = *static_cast<Staging*>(context);
    if (out.rows == MAX_ROWS) {
      LOG_ERR("BookOrbit", "More than %d books on the card", MAX_ROWS);
      return false;
    }
    if (path.size() > PATH_BYTES) return true;  // longer than the index allows: never a catalog book
    memset(out.record, 0, PATH_BYTES);
    memcpy(out.record, path.data(), path.size());
    if (out.paths.write(out.record, PATH_BYTES) != PATH_BYTES) return false;
    out.rows++;
    return true;
  };
  const bool walked = walk(*scratch, stage, &staging);
  const bool written = paths.close() && walked;
  bool ok = written && staging.rows > 0 && Storage.openFileForRead("BookOrbit", PATHS_FILE, paths) &&
            sortRecords(paths, static_cast<uint16_t>(staging.rows), *scratch);
  paths.close();
  if (written && staging.rows == 0) {
    clear();
    ok = true;
  }
  if (!ok) {
    LOG_ERR("BookOrbit", "Could not build the On device list (%d books)", staging.rows);
    clear();
    return false;
  }
  total = staging.rows;
  LOG_INF("BookOrbit", "On device list built: %d books", total);
  return true;
}

bool BookOrbitLocalBooks::readPage(const int firstRow, const size_t count, std::vector<std::string>& paths) {
  auto record = makeUniqueNoThrow<char[]>(PATH_BYTES + 1);
  FsFile records;
  FsFile order;
  bool ok = record && firstRow >= 0 && Storage.openFileForRead("BookOrbit", PATHS_FILE, records) &&
            Storage.openFileForRead("BookOrbit", ORDER_FILE, order) &&
            order.seekSet(static_cast<size_t>(firstRow) * sizeof(uint16_t));
  paths.reserve(count);
  for (size_t i = 0; ok && i < count; i++) {
    uint16_t row = 0;
    ok = serialization::tryReadPod(order, row) && readRecord(records, row, record.get());
    if (ok) paths.emplace_back(record.get());
  }
  order.close();
  records.close();
  if (!ok) LOG_ERR("BookOrbit", "Could not read the On device list from row %d", firstRow);
  return ok;
}

void BookOrbitLocalBooks::clear() {
  for (const char* path : {PATHS_FILE, ORDER_FILE, RUN_FILE}) {
    if (Storage.exists(path)) Storage.remove(path);
  }
}
