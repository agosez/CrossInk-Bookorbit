#pragma once

#include <cstddef>

class InflateStream {
 public:
  static constexpr size_t requiredInternalStorageSize(bool) { return 32768; }
};
