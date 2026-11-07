#include <HalScreenCalibration.h>
#include <nvs.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

namespace {
std::vector<uint8_t> saved, pending;
int openError = ESP_OK, readError = ESP_OK, writeError = ESP_OK, commitError = ESP_OK;
int handles = 0, writes = 0;
void reset() {
  saved.clear();
  pending.clear();
  openError = readError = writeError = commitError = ESP_OK;
  handles = writes = 0;
}
}  // namespace

esp_err_t nvs_open(const char* name, int, nvs_handle_t* handle) {
  assert(std::strcmp(name, "screen-cal") == 0);
  if (openError) return openError;
  *handle = 1;
  ++handles;
  return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* buffer, size_t* length) {
  assert(std::strcmp(key, "bounds") == 0);
  if (readError) return readError;
  if (saved.empty()) return ESP_ERR_NVS_NOT_FOUND;
  if (*length < saved.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  *length = saved.size();
  std::memcpy(buffer, saved.data(), saved.size());
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void* buffer, size_t length) {
  ++writes;
  if (writeError) return writeError;
  const auto* bytes = static_cast<const uint8_t*>(buffer);
  pending.assign(bytes, bytes + length);
  return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
  if (commitError) return commitError;
  saved = pending;
  return ESP_OK;
}
void nvs_close(nvs_handle_t) {
  --handles;
  pending.clear();
}

int main() {
  const ScreenInsets defaults;
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  assert((defaults.edges == std::array<uint8_t, 4>{{0, 0, 0, 0}}));
#else
  assert((defaults.edges == std::array<uint8_t, 4>{{9, 3, 3, 3}}));
#endif
  const ScreenInsets calibrated{{{4, 12, 20, 32}}};
  const std::array<std::array<uint8_t, 4>, 4> expected{
      {{{4, 12, 20, 32}}, {{32, 4, 12, 20}}, {{20, 32, 4, 12}}, {{12, 20, 32, 4}}}};
  for (unsigned rotation = 0; rotation < 4; ++rotation) {
    assert(calibrated.rotated(rotation).edges == expected[rotation]);
    assert(calibrated.rotated(rotation).rotated(4 - rotation) == calibrated);
  }
  // Default and custom geometry both keep status content inside each physical edge.
  const ScreenInsets maximum{{{32, 32, 32, 32}}};
  for (unsigned rotation = 0; rotation < 4; ++rotation) {
    for (const int legacy : {-5, 0, 5, 10}) {
      assert(defaults.topOrigin(rotation, legacy) == std::max<int>(defaults.rotated(rotation).edges[0], legacy));
      const int top = maximum.rotated(rotation).edges[0];
      const int adjusted = legacy + top - defaults.rotated(rotation).edges[0];
      assert(maximum.topOrigin(rotation, legacy) == (adjusted < top ? top : adjusted));
      assert(maximum.topOrigin(rotation, legacy) >= top);
    }
  }
  ScreenInsets draft = calibrated;
  draft.adjust(0, -100);
  draft.adjust(1, 100);
  draft.adjust(4, 1);
  assert(draft.edges[0] == 0 && draft.edges[1] == 32);
  assert(calibrated.edges[0] == 4 && calibrated.edges[1] == 12);
  assert(draft.valid());
  reset();
  assert(HalScreenCalibration::load() == defaults);
  assert(handles == 0);
  openError = ESP_FAIL;
  assert(HalScreenCalibration::load() == defaults && handles == 0);
  openError = ESP_ERR_NVS_NOT_FOUND;
  assert(HalScreenCalibration::load() == defaults && handles == 0);
  openError = ESP_OK;
  assert(HalScreenCalibration::save(calibrated));
  assert(HalScreenCalibration::load() == calibrated);
  assert(saved.size() == 8 && saved[2] == 1 && handles == 0);
  const auto original = saved;
  // Draft edits are not persisted until Save; reset is also only a draft.
  draft = HalScreenCalibration::load();
  draft.adjust(2, 1);
  assert(HalScreenCalibration::load() == calibrated);
  draft = defaults;
  assert(HalScreenCalibration::load() == calibrated);
  for (int failure = 0; failure < 3; ++failure) {
    openError = failure == 0 ? ESP_FAIL : ESP_OK;
    writeError = failure == 1 ? ESP_FAIL : ESP_OK;
    commitError = failure == 2 ? ESP_FAIL : ESP_OK;
    assert(!HalScreenCalibration::save(defaults));
    assert(handles == 0 && saved == original);
  }
  openError = writeError = commitError = ESP_OK;
  readError = ESP_FAIL;
  assert(HalScreenCalibration::load() == defaults && handles == 0);
  readError = ESP_OK;
  for (size_t byte = 0; byte < original.size(); ++byte) {
    saved = original;
    saved[byte] ^= 1;
    assert(HalScreenCalibration::load() == defaults && handles == 0);
  }
  saved = original;
  saved[2] = 99;
  saved[7] ^= original[2] ^ 99;  // Future version with otherwise valid encoding.
  assert(HalScreenCalibration::load() == defaults);
  saved = original;
  saved[3] = 33;
  saved[7] ^= original[3] ^ 33;  // Valid checksum, invalid range.
  assert(HalScreenCalibration::load() == defaults);
  saved = original;
  saved.push_back(0);
  assert(HalScreenCalibration::load() == defaults);
  saved.resize(3);
  assert(HalScreenCalibration::load() == defaults);
  ScreenInsets invalid;
  invalid.edges[0] = 255;
  const int beforeWrites = writes;
  assert(!HalScreenCalibration::save(invalid) && writes == beforeWrites);
  assert(HalScreenCalibration::save(defaults));
  assert(HalScreenCalibration::load() == defaults && handles == 0);
}
