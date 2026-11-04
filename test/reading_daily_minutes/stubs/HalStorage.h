#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int O_RDONLY = 0;
constexpr int O_RDWR = 2;
constexpr int O_CREAT = 0x40;
constexpr int O_TRUNC = 0x200;

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::shared_ptr<std::vector<uint8_t>> data) : data_(std::move(data)) {}

  bool seekSet(const size_t position) {
    if (!data_ || position > data_->size()) return false;
    cursor_ = position;
    return true;
  }
  int read(uint8_t* output, const size_t length) {
    if (!data_) return 0;
    const size_t readable = std::min(length, data_->size() - cursor_);
    std::copy_n(data_->data() + cursor_, readable, output);
    cursor_ += readable;
    return static_cast<int>(readable);
  }
  size_t write(const uint8_t* input, const size_t length) {
    if (!data_) return 0;
    if (cursor_ + length > data_->size()) data_->resize(cursor_ + length);
    std::copy_n(input, length, data_->begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += length;
    return length;
  }
  size_t size() const { return data_ ? data_->size() : 0; }
  void close() {
    data_.reset();
    cursor_ = 0;
  }
  explicit operator bool() const { return static_cast<bool>(data_); }

 private:
  std::shared_ptr<std::vector<uint8_t>> data_;
  size_t cursor_ = 0;
};

class HalStorage {
 public:
  void reset() { files_.clear(); }
  bool exists(const char* path) const { return files_.count(path) != 0; }
  bool remove(const char* path) { return files_.erase(path) != 0; }
  HalFile open(const char* path, const int flags = O_RDONLY) {
    auto found = files_.find(path);
    if (found == files_.end()) {
      if ((flags & O_CREAT) == 0) return HalFile();
      found = files_.emplace(path, std::make_shared<std::vector<uint8_t>>()).first;
    }
    if ((flags & O_TRUNC) != 0) found->second->clear();
    return HalFile(found->second);
  }
  std::vector<uint8_t>* bytes(const char* path) {
    const auto found = files_.find(path);
    return found == files_.end() ? nullptr : found->second.get();
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> files_;
};

inline HalStorage Storage;
