#include "BookOrbitDevicePath.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <vector>

#include "StringUtils.h"

namespace {

constexpr char EPUB_EXTENSION[] = ".epub";
constexpr size_t EPUB_EXTENSION_LEN = sizeof(EPUB_EXTENSION) - 1;

bool isSeparator(const char c) { return c == '/' || c == '\\'; }

// A segment made of nothing but spaces and dots must never reach the sanitizer: ".."
// would climb out of the download folder, and the sanitizer answers an empty segment
// with its "book" placeholder, which would create a junk directory.
bool isEmptyOrTraversal(const std::string& segment) { return segment.find_first_not_of(" .") == std::string::npos; }

bool endsWithEpubExtension(const std::string& name) {
  if (name.size() <= EPUB_EXTENSION_LEN) return false;

  return std::equal(name.end() - EPUB_EXTENSION_LEN, name.end(), EPUB_EXTENSION,
                    [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

size_t joinedLength(const std::vector<std::string>& segments) {
  if (segments.empty()) return 0;

  // Every segment's bytes, on top of the separators between them.
  return std::accumulate(segments.begin(), segments.end(), segments.size() - 1,
                         [](const size_t total, const std::string& segment) { return total + segment.size(); });
}

std::string join(const std::vector<std::string>& segments) {
  std::string path;
  path.reserve(joinedLength(segments));
  for (const std::string& segment : segments) {
    if (!path.empty()) path += '/';
    path += segment;
  }
  return path;
}

}  // namespace

namespace BookOrbitDevicePath {

std::string sanitizeRelativePath(const std::string& devicePath) {
  std::vector<std::string> segments;
  std::string raw;
  // One pass over the path, flushing a segment at every separator and at the end. A
  // leading separator (an absolute path from the server) yields an empty first
  // segment, which is dropped like any other empty one.
  for (size_t i = 0; i <= devicePath.size(); i++) {
    if (i < devicePath.size() && !isSeparator(devicePath[i])) {
      raw += devicePath[i];
      continue;
    }
    if (!raw.empty() && !isEmptyOrTraversal(raw)) {
      std::string clean = StringUtils::sanitizeFilename(raw);
      if (!clean.empty()) segments.push_back(std::move(clean));
    }
    raw.clear();
  }
  if (segments.empty()) return "";

  std::string filename = std::move(segments.back());
  segments.pop_back();
  if (!endsWithEpubExtension(filename)) filename += EPUB_EXTENSION;

  // Shed directories, deepest first, until the path fits — the only bound on nesting.
  // A template groups from the top down ("Series/<name>"), so what survives is the
  // grouping. The filename always fits: the sanitizer caps a segment below the limit.
  segments.push_back(std::move(filename));
  while (segments.size() > 1 && joinedLength(segments) > MAX_RELATIVE_PATH_BYTES) {
    segments.erase(segments.end() - 2);
  }
  return join(segments);
}

}  // namespace BookOrbitDevicePath
