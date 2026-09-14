#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr uint8_t ESP_PARTITION_TYPE_APP = 0;
constexpr uint8_t ESP_PARTITION_TYPE_DATA = 1;
constexpr uint8_t ESP_PARTITION_SUBTYPE_DATA_OTA = 0;
constexpr uint8_t ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10;
struct esp_partition_t {
  uint8_t type;
  uint8_t subtype;
  uint32_t size;
  char label[17];
};
const esp_partition_t* esp_partition_find_first(uint8_t type, uint8_t subtype, const char* label);
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t, size_t);
esp_err_t esp_partition_write(const esp_partition_t*, size_t, const void*, size_t);
