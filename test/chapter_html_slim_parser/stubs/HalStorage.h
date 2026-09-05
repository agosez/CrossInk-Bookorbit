#pragma once

#include <Print.h>

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile : public Print {
 public:
  int available() const { return 0; }
  size_t position() const { return 0; }
  size_t size() const { return 0; }
  int read(void*, size_t) { return 0; }
  bool seek(size_t) { return false; }
  bool seekSet(size_t) { return false; }
  bool close() { return true; }
  bool isOpen() const { return false; }
  operator bool() const { return false; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) { return 0; }
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char*, const std::string&, HalFile&) { return false; }
  bool openFileForWrite(const char*, const std::string&, HalFile&) { return false; }
  bool remove(const char*) { return false; }
  bool exists(const char*) const { return false; }
  bool rename(const char*, const char*) { return false; }
};

#define Storage HalStorage::getInstance()
