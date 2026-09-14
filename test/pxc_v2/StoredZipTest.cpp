#include <ZipFile.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) {                                              \
      fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); \
      exit(1);                                               \
    }                                                        \
  } while (0)
struct Output : Print {
  size_t count = 0, limit = 99999;
  size_t write(uint8_t) override {
    if (count == limit) return 0;
    ++count;
    return 1;
  }
};
int main() {
  std::ifstream f(GOLDEN_DIR "/stored-vs-deflated.zip", std::ios::binary);
  CHECK(f.good());
  Storage.put("test.epub", {std::istreambuf_iterator<char>(f), {}});
  Output stored;
  CHECK(ZipFile("test.epub").readStoredFileToStream("stored.pxc2", stored));
  CHECK(stored.count > 32);
  Output deflated;
  CHECK(!ZipFile("test.epub").readStoredFileToStream("deflated.pxc2", deflated));
  CHECK(deflated.count == 0);
  CHECK(InflateStream::initCalls == 0);
  Output failed;
  failed.limit = 19;
  CHECK(!ZipFile("test.epub").readStoredFileToStream("stored.pxc2", failed));
  CHECK(!ZipFile("test.epub").readStoredFileToStream("absent.pxc2", failed));
  puts("Stored-only ZIP rejects deflated PXC2 without initializing an inflater");
}
