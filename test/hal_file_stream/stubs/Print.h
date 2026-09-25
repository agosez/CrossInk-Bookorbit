#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
class String {};
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* data, size_t size) {
    size_t written = 0;
    while (size--) written += write(*data++);
    return written;
  }
};
