#pragma once

#include <Arduino.h>

#include <string>
#include <unordered_map>

class HalStorage {
 public:
  void reset() {
    files.clear();
    failRenameFrom.clear();
  }

  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) { return files.erase(path) > 0; }

  bool rename(const char* from, const char* to) {
    if (failRenameFrom == from) {
      failRenameFrom.clear();
      return false;
    }
    const auto source = files.find(from);
    if (source == files.end() || files.contains(to)) return false;
    files[to] = source->second;
    files.erase(source);
    return true;
  }

  bool writeFile(const char* path, const String& content) {
    files[path] = content.c_str();
    return true;
  }

  String readFile(const char* path) const {
    const auto file = files.find(path);
    return file == files.end() ? String() : String(file->second.c_str());
  }

  void put(const std::string& path, std::string content) { files[path] = std::move(content); }
  void failNextRenameFrom(std::string path) { failRenameFrom = std::move(path); }

 private:
  std::unordered_map<std::string, std::string> files;
  std::string failRenameFrom;
};

inline HalStorage Storage;
