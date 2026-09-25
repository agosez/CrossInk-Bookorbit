#pragma once

#include <cstddef>

class HalFile {
 public:
  explicit operator bool() const { return false; }
  bool isDirectory() const { return false; }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return false; }
  HalFile openNextFile() { return {}; }
  void getName(char*, size_t) const {}
  void close() {}
};

class HalStorage {
 public:
  HalFile open(const char*) { return {}; }
};

inline HalStorage Storage;
inline void yield() {}
