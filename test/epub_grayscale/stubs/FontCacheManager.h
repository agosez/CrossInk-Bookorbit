#pragma once
class FontCacheManager {
 public:
  bool scanning = false;
  bool isScanning() const { return scanning; }
};
