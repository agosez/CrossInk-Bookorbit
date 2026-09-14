#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "OptimizerCachePublish.h"
#include "OptimizerIndex.h"
#include "PxcV2.h"
using namespace OptimizerFormat;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) {                                              \
      fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); \
      exit(1);                                               \
    }                                                        \
  } while (0)
struct Output : Print {
  std::vector<uint8_t> bytes;
  size_t limit = 1000000;
  size_t write(uint8_t b) override {
    if (bytes.size() == limit) return 0;
    bytes.push_back(b);
    return 1;
  }
};
std::vector<uint8_t> load(const char* name) {
  std::ifstream f(std::string(GOLDEN_DIR) + "/" + name, std::ios::binary);
  CHECK(f.good());
  return {std::istreambuf_iterator<char>(f), {}};
}
Record record(const std::vector<uint8_t>& data) {
  Record r;
  strcpy(r.href, "EPUB/image.jpg");
  strcpy(r.pxcHref, "META-INF/crossink/pxc/test.pxc2");
  r.width = u16(data.data() + 8);
  r.height = u16(data.data() + 10);
  r.format = 2;
  r.bytes = data.size();
  r.pixelCrc = u32(data.data() + 24);
  return r;
}
bool decode(const std::vector<uint8_t>& data, const Record& r, Output& output, int w = 127, int h = 131) {
  PxcV2Workspace ws;
  PxcV2 decoder(ws, output, r, w, h);
  for (size_t offset = 0; offset < data.size();) {
    const size_t n = std::min<size_t>(17, data.size() - offset);
    if (decoder.write(data.data() + offset, n) != n) return false;
    offset += n;
  }
  return decoder.finish();
}
struct IndexInput {
  std::vector<uint8_t> bytes;
  size_t calls = 0, failAt = SIZE_MAX;
};
bool readIndex(void* context, uint32_t offset, uint8_t* bytes, size_t n) {
  auto& input = *static_cast<IndexInput*>(context);
  if (input.calls++ == input.failAt || offset + n > input.bytes.size()) return false;
  memcpy(bytes, input.bytes.data() + offset, n);
  return true;
}
bool checkIndex(IndexInput& input) {
  IndexScratch scratch;
  uint16_t count = 999;
  input.calls = 0;
  return validateIndex(readIndex, &input, input.bytes.size(), 42, 12973, scratch, count);
}
void testIndexes() {
  for (uint16_t count : {0, 1, 89, 256}) {
    IndexInput input;
    input.bytes.resize(32 + 208 * count);
    for (uint16_t i = 0; i < count; ++i) {
      Record r;
      snprintf(r.href, sizeof(r.href), "EPUB/%u.jpg", i);
      strcpy(r.pxcHref, "META-INF/crossink/pxc/a.pxc");
      r.width = r.height = 32;
      r.bytes = 260;
      encodeRecord(r, input.bytes.data() + 32 + 208 * i);
    }
    auto header = [&]() {
      indexHeader(input.bytes.data(), count, 42, 12973, crc(0, input.bytes.data() + 32, input.bytes.size() - 32));
    };
    header();
    CHECK(checkIndex(input));
    for (size_t fail = 0; fail <= count; ++fail) {
      input.failAt = fail;
      CHECK(!checkIndex(input));
    }
    input.failAt = SIZE_MAX;
    if (count > 1) {
      memcpy(input.bytes.data() + 32 + 208, input.bytes.data() + 32, 208);
      header();
      CHECK(!checkIndex(input));
    } else if (count) {
      input.bytes[32] = '/';
      header();
      CHECK(!checkIndex(input));
    }
  }
}
struct PublishFs {
  std::map<std::string, int> files{{"final", 1}, {"tmp", 2}};
  unsigned renames = 0;
  std::set<unsigned> failures;
  bool exists(const char* path) { return files.count(path); }
  bool remove(const char* path) { return files.erase(path); }
  bool rename(const char* from, const char* to) {
    if (failures.count(++renames) || !exists(from) || exists(to)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};
struct SyncOutput {
  bool syncOk = true, closeOk = true, closed = false;
  bool sync() { return syncOk; }
  bool close() {
    closed = true;
    return closeOk;
  }
};
void testPublishing() {
  {
    PublishFs fs;
    fs.files["backup"] = 0;
    SyncOutput output;
    CHECK(finishCache(output, fs, true, "tmp", "final", "backup"));
    CHECK(fs.files["final"] == 2 && !fs.exists("backup"));
  }
  {
    PublishFs fs;
    SyncOutput output;
    output.closeOk = false;
    CHECK(!finishCache(output, fs, true, "tmp", "final", "backup"));
    CHECK(output.closed && fs.renames == 0 && fs.files["final"] == 1);
  }

  for (unsigned fail : {0, 1, 2}) {
    PublishFs fs;
    fs.failures.insert(fail);
    SyncOutput output;
    bool ok = finishCache(output, fs, true, "tmp", "final", "backup");
    CHECK(output.closed);
    CHECK(ok == (fail == 0));
    CHECK(fs.files["final"] == (ok ? 2 : 1));
  }
  PublishFs fs;
  fs.failures = {2, 3};
  SyncOutput output;
  CHECK(!finishCache(output, fs, true, "tmp", "final", "backup"));
  CHECK(fs.files["backup"] == 1);
  CHECK(!fs.exists("final"));
  fs.failures.clear();
  CHECK(recoverCache(fs, "final", "backup"));
  CHECK(fs.files["final"] == 1);
  for (bool valid : {false, true}) {
    PublishFs noSync;
    SyncOutput bad;
    bad.syncOk = false;
    CHECK(!finishCache(bad, noSync, valid, "tmp", "final", "backup"));
    CHECK(bad.closed && noSync.renames == 0 && noSync.files["final"] == 1);
  }
}
int main() {
  testIndexes();
  testPublishing();
  const auto raw = load("pixels.pxc");
  CHECK(dimensions(1024, 512));
  CHECK(!dimensions(1024, 513));
  CHECK(!dimensions(65535, 65535));
  {
    auto data = load("deflate.pxc2");
    data[24] ^= 1;
    Output rejected;
    CHECK(!decode(data, record(data), rejected));
  }
  {
    uint32_t random = 123;
    const auto original = load("deflate.pxc2");
    for (int i = 0; i < 3000; ++i) {
      auto data = original;
      random = random * 1664525U + 1013904223U;
      const size_t at = 44 + random % u16(data.data() + 36);
      data[at] ^= 1U << ((random >> 16) & 7);
      Output output;
      if (decode(data, record(data), output)) CHECK(output.bytes == raw);
    }
  }

  {
    auto data = load("deflate.pxc2");
    // A valid DEFLATE stream plus extra encoded input must be rejected even
    // when the outer lengths and decoded CRC still match.
    const uint16_t encoded = u16(data.data() + 36);
    data.insert(data.begin() + 44 + encoded, 0);
    put16(data.data() + 36, encoded + 1);
    put32(data.data() + 28, data.size());
    Output rejected;
    CHECK(!decode(data, record(data), rejected));
  }
  {
    const auto mixed = load("mixed.pxc2");
    std::set<uint8_t> codecs;
    for (size_t at = 32; at < mixed.size(); at += 12 + u16(mixed.data() + at + 4)) codecs.insert(mixed[at]);
    CHECK(codecs == (std::set<uint8_t>{0, 1, 2}));
  }

  for (auto name : {"raw.pxc2", "deflate.pxc2", "packbits.pxc2", "mixed.pxc2", "browser.pxc2"}) {
    auto data = load(name);
    const auto r = record(data);
    Output output;
    CHECK(decode(data, r, output));
    CHECK(output.bytes == raw);
    for (auto size : {std::pair<int, int>{63, 67}, {254, 262}, {1, 1}}) {
      Output resized;
      CHECK(decode(data, r, resized, size.first, size.second));
      CHECK(resized.bytes.size() == 4U + (size.first + 3) / 4 * size.second);
      for (int y = 0; y < size.second; ++y)
        for (int x = 0; x < size.first; ++x) {
          const int sx = x * 127 / size.first, sy = y * 131 / size.second;
          CHECK(((resized.bytes[4 + y * ((size.first + 3) / 4) + x / 4] >> (6 - 2 * (x % 4))) & 3) ==
                ((raw[4 + sy * 32 + sx / 4] >> (6 - 2 * (sx % 4))) & 3));
        }
    }
    for (size_t i = 0; i < 44; ++i) {
      auto bad = data;
      bad[i] ^= 0x80;
      Output rejected;
      CHECK(!decode(bad, r, rejected));
    }
    for (size_t n = 0; n < data.size(); ++n) {
      std::vector<uint8_t> truncated(data.begin(), data.begin() + n);
      Output rejected;
      CHECK(!decode(truncated, r, rejected));
    }
    auto trailing = data;
    trailing.push_back(0);
    Output rejected;
    CHECK(!decode(trailing, r, rejected));
    Output full;
    full.limit = 40;
    CHECK(!decode(data, r, full));
  }
  for (uint16_t count : {0, 1, 89, 256}) {
    uint8_t header[32];
    indexHeader(header, count, 42, 12973, 0);
    CHECK(validHeader(header, 42, 12973, 32 + 208 * count));
    CHECK(!validHeader(header, 43, 12973, 32 + 208 * count));
    CHECK(!validHeader(header, 42, 12973, 33 + 208 * count));
    for (size_t i = 0; i < 32; ++i) {
      header[i] ^= 1;
      CHECK(!validHeader(header, 42, 12973, 32 + 208 * count));
      header[i] ^= 1;
    }
  }
  uint8_t header[32];
  indexHeader(header, 257, 42, 12973, 0);
  CHECK(!validHeader(header, 42, 12973, 32 + 208 * 257));
  auto r = record(load("mixed.pxc2"));
  uint8_t bytes[208];
  encodeRecord(r, bytes);
  Record parsed;
  CHECK(decodeRecord(bytes, parsed));
  CHECK(!strcmp(parsed.href, r.href));
  memset(bytes, 'a', 129);
  CHECK(!decodeRecord(bytes, parsed));
  for (const char* bad : {"../x", "/x", "a\\b", "a:%00", ""}) {
    strcpy(r.href, bad);
    CHECK(!valid(r));
  }
  printf("PXC2 golden, rescale, truncation, corruption, write-failure and COIX tests passed; workspace=%zu\n",
         sizeof(PxcV2Workspace));
}
