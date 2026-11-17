#include "BookOrbitKeySet.h"

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
  for (const BookOrbitAnnotationRecord& record : records) {
    if (!appendKey(keys, record.identityEpoch, record.pos0)) {
      keys.clear();
      return false;
    }
  }
  return true;
}

bool build(const std::vector<BookOrbitBookmarkRecord>& records, std::vector<BookOrbitAnnotationKey>& keys) {
  keys.reserve(records.size());
  for (const BookOrbitBookmarkRecord& record : records) {
    if (!appendKey(keys, record.identityEpoch, record.pos)) {
      keys.clear();
      return false;
    }
  }
  return true;
}

}  // namespace BookOrbitKeySet
