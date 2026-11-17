#include "BookOrbitKeySet.h"

#include <algorithm>

namespace BookOrbitKeySet {
namespace {
bool appendKey(std::vector<BookOrbitAnnotationKey>& keys, const uint32_t identityEpoch, const std::string& pos) {
  BookOrbitAnnotationKey key;
  if (!bookOrbitFormatDatetime(identityEpoch, key.dt) || !bookOrbitAnnotationKey(key.dt, pos.c_str(), key.k)) {
    return false;
  }
  keys.push_back(key);
  return true;
}
}  // namespace

bool build(const std::vector<BookOrbitAnnotationRecord>& records, std::vector<BookOrbitAnnotationKey>& keys) {
  keys.reserve(records.size());
  const bool ok = std::all_of(records.begin(), records.end(), [&keys](const BookOrbitAnnotationRecord& record) {
    return appendKey(keys, record.identityEpoch, record.pos0);
  });
  if (!ok) keys.clear();
  return ok;
}

bool build(const std::vector<BookOrbitBookmarkRecord>& records, std::vector<BookOrbitAnnotationKey>& keys) {
  keys.reserve(records.size());
  const bool ok = std::all_of(records.begin(), records.end(), [&keys](const BookOrbitBookmarkRecord& record) {
    return appendKey(keys, record.identityEpoch, record.pos);
  });
  if (!ok) keys.clear();
  return ok;
}

}  // namespace BookOrbitKeySet
