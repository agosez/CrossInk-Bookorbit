#pragma once
#include <cstddef>
#include <cstdint>
class InflateStream {
 public:
  enum class Status { Done, Ok, Error };
  inline static unsigned initCalls = 0;
  bool init(bool) {
    ++initCalls;
    return false;
  }
  void deinit() {}
  void setSource(const uint8_t*, size_t) {}
  void setFill(size_t (*)(void*, const uint8_t**), void*) {}
  bool read(uint8_t*, size_t) { return false; }
  Status readAtMost(uint8_t*, size_t, size_t* produced) {
    *produced = 0;
    return Status::Error;
  }
};
