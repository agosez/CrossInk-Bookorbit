#include "BookOrbitLocalBooks.h"

#include <BookOrbitCredentialStore.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibrarySort.h>
#include <Logging.h>
#include <Memory.h>
#include <NaturalSort.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "BookOrbitDownloadIndex.h"

namespace {

constexpr char READ_FOLDER[] = "/Read";
constexpr char PATHS_FILE[] = "/.crosspoint/bookorbit_local.paths";
constexpr char ORDER_FILE[] = "/.crosspoint/bookorbit_local.order";
constexpr char RUN_FILE[] = "/.crosspoint/bookorbit_local.sort";
constexpr int MAX_ROWS = INT16_MAX;
constexpr size_t PATH_BYTES = 512;

struct Scratch {
  // Reused for directory names, then for the staged absolute path and sort key.
  char path[PATH_BYTES]{};
  uint8_t key[512]{};
};

std::string_view parentOf(const std::string_view path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string_view::npos ? std::string_view{} : path.substr(0, std::max<size_t>(1, slash));
}

template <typename Visit>
bool scan(Scratch& scratch, Visit visit) {
  const auto& folder = BOOKORBIT_STORE.getDownloadFolder();
  // Indexed books in these directories will be picked up by the scan, even
  // when a replacement on disk has invalidated the index's recorded size.
  struct IndexedBooks {
    const std::string& folder;
    Visit& visit;
    bool ok = true;
  } indexed{folder, visit};
  BookOrbitDownloadIndex::forEachExisting(
      [](int64_t, const std::string& path, void* context) {
        auto& scan = *static_cast<IndexedBooks*>(context);
        const auto parent = parentOf(path);
        if (scan.ok && !parent.empty() && parent != "/" && parent != READ_FOLDER && parent != scan.folder &&
            FsHelpers::hasEpubExtension(path)) {
          scan.ok = scan.visit(path);
        }
      },
      &indexed);
  if (!indexed.ok) return false;

  const auto scanDir = [&](const std::string& dirPath) {
    FsFile dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      const bool ok = !dir.allocationFailed();
      dir.close();
      return ok;
    }
    const std::string prefix = dirPath == "/" ? dirPath : dirPath + "/";
    std::string path;
    path.reserve(sizeof(scratch.path) - 1);
    bool ok = true;
    FsFile file;
    while (ok && (file = dir.openNextFile())) {
      const size_t nameLength = file.isDirectory() ? 0 : file.getName(scratch.path, sizeof(scratch.path));
      file.close();
      if (nameLength <= 5 || nameLength >= sizeof(scratch.path) ||
          !FsHelpers::hasEpubExtension(std::string_view(scratch.path, nameLength))) {
        continue;
      }
      if (prefix.size() + nameLength >= sizeof(scratch.path)) {
        LOG_ERR("BookOrbit", "Skipping local EPUB with a path longer than the snapshot record");
        continue;
      }
      path.assign(prefix).append(scratch.path, nameLength);
      ok = visit(path);
    }
    const bool complete = ok && !dir.iterationFailed() && !file.allocationFailed();
    dir.close();
    return complete;
  };
  return (folder.empty() || folder == "/" || folder == READ_FOLDER || scanDir(folder)) && scanDir("/") &&
         scanDir(READ_FOLDER);
}

bool readPath(FsFile& file, const uint16_t ordinal, char (&path)[PATH_BYTES]) {
  return file.seekSet(static_cast<size_t>(ordinal) * sizeof(path)) && file.read(path, sizeof(path)) == sizeof(path) &&
         path[sizeof(path) - 1] == '\0' && path[0] == '/' && FsHelpers::hasEpubExtension(std::string_view(path));
}

size_t makeKey(Scratch& scratch) {
  char* name = strrchr(scratch.path, '/') + 1;
  name[strlen(name) - 5] = '\0';
  return FsHelpers::naturalSortKey(name, scratch.key, sizeof(scratch.key));
}

}  // namespace

size_t BookOrbitLocalBooks::count() {
  // This scratch buffer is too large for an activity's stack.
  auto scratch = makeUniqueNoThrow<Scratch>();
  if (!scratch) {
    LOG_ERR("BookOrbit", "Could not allocate local book scan buffer");
    return 0;
  }
  size_t found = 0;
  if (!scan(*scratch, [&](const std::string&) {
        ++found;
        return true;
      })) {
    LOG_ERR("BookOrbit", "Could not finish counting local books");
  }
  LOG_DBG("BookOrbit", "Local scan: kind=on-device count=%u", static_cast<unsigned>(found));
  return found;
}

bool BookOrbitLocalBooks::build(int& total) {
  clear();
  total = 0;
  auto scratch = makeUniqueNoThrow<Scratch>();
  FsFile paths;
  if (!scratch || !Storage.openFileForWrite("BookOrbit", PATHS_FILE, paths)) {
    LOG_ERR("BookOrbit", "Could not prepare local book snapshot");
    return false;
  }
  const bool scanned = scan(*scratch, [&](const std::string& path) {
    if (total == MAX_ROWS) return false;
    memset(scratch->path, 0, sizeof(scratch->path));
    memcpy(scratch->path, path.data(), path.size());
    if (paths.write(scratch->path, sizeof(scratch->path)) != sizeof(scratch->path)) return false;
    ++total;
    return true;
  });
  const bool closed = paths.close();
  if (!scanned || !closed) {
    LOG_ERR("BookOrbit", "Could not stage local book paths (%d rows)", total);
    clear();
    return false;
  }
  if (total == 0) return true;

  struct SortInput {
    FsFile& paths;
    Scratch& scratch;
  } input{paths, *scratch};
  library::SortConfig config;
  config.runPath = RUN_FILE;
  config.keyBytes = sizeof(scratch->key);
  config.loadContext = &input;
  config.load = [](void* context, const uint16_t source, const size_t offset, char* segment, size_t& length) {
    auto& input = *static_cast<SortInput*>(context);
    if (!readPath(input.paths, source, input.scratch.path)) return false;
    length = makeKey(input.scratch);
    memset(segment, 0, library::SORT_SEGMENT_BYTES);
    if (offset < length) {
      memcpy(segment, input.scratch.key + offset, std::min(length - offset, library::SORT_SEGMENT_BYTES));
    }
    return true;
  };
  library::ExternalSorter sorter;
  FsFile order;
  bool ok =
      Storage.openFileForRead("BookOrbit", PATHS_FILE, paths) && sorter.begin(config, static_cast<uint16_t>(total));
  for (uint16_t row = 0; ok && row < total; ++row) {
    ok = readPath(paths, row, scratch->path);
    if (ok) {
      library::SortEntry entry{};
      entry.source = row;
      entry.ordinal = row;
      const size_t length = makeKey(*scratch);
      memcpy(entry.key, scratch->key, std::min(length, sizeof(entry.key)));
      ok = sorter.add(entry);
    }
  }
  if (ok && Storage.openFileForWrite("BookOrbit", ORDER_FILE, order)) {
    ok = sorter.finish(
        [](void* context, const library::SortEntry& entry) {
          return serialization::tryWritePod(*static_cast<FsFile*>(context), entry.ordinal);
        },
        &order);
    ok = order.close() && ok;
  } else {
    ok = false;
  }
  paths.close();
  if (!ok) {
    LOG_ERR("BookOrbit", "Could not sort local book snapshot");
    // The sorter owns the run file until its destructor.
    Storage.remove(PATHS_FILE);
    Storage.remove(ORDER_FILE);
  }
  return ok;
}

bool BookOrbitLocalBooks::readPage(const int firstRow, const size_t count, std::vector<std::string>& result) {
  auto scratch = makeUniqueNoThrow<Scratch>();
  FsFile paths;
  FsFile order;
  bool ok = scratch && Storage.openFileForRead("BookOrbit", PATHS_FILE, paths) &&
            Storage.openFileForRead("BookOrbit", ORDER_FILE, order) &&
            order.seekSet(static_cast<size_t>(firstRow) * sizeof(uint16_t));
  result.reserve(count);
  for (size_t i = 0; ok && i < count; ++i) {
    uint16_t ordinal = 0;
    ok = serialization::tryReadPod(order, ordinal) && readPath(paths, ordinal, scratch->path);
    if (ok) result.emplace_back(scratch->path);
  }
  order.close();
  paths.close();
  if (!ok) LOG_ERR("BookOrbit", "Could not read local books at row %d", firstRow);
  return ok;
}

void BookOrbitLocalBooks::clear() {
  Storage.remove(PATHS_FILE);
  Storage.remove(ORDER_FILE);
  Storage.remove(RUN_FILE);
}
