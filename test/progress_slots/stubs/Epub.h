#pragma once
#include <string>
class Epub {
 public:
  // The book's own path, which this fork hashes for the content-keyed progress copy.
  const std::string& getPath() const {
    static const std::string path = "/book.epub";
    return path;
  }
  const std::string& getCachePath() const {
    static const std::string path = "/epub";
    return path;
  }
};
