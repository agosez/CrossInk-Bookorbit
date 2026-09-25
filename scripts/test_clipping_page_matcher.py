#!/usr/bin/env python3
"""Run the EPUB clipping matcher with small host page/storage fixtures.

Compile the actual reader helpers, extracted between named function boundaries,
so the regression exercises traversal and neighbor verification without requiring
an entire firmware activity, renderer, or SD card.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

FIXTURES = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include "clippings/ClippingTextMatcher.h"
#include "clippings/ClippingMatchTracker.h"
#include "clippings/ClippingHighlightGeometry.h"
#define LOG_ERR(...) ((void)0)
struct TableFragmentRow { static constexpr uint8_t MAX_SERIALIZED_CELLS = 8; };
struct TextBlock {
  std::vector<std::string> words;
  uint16_t wordCount() const { return words.size(); }
  const char* wordText(size_t i) const { return words[i].c_str(); }
  bool wordEndsWithInsertedHyphen(size_t i) const { return words[i].back() == '-'; }
};
struct PageTextLine { const TextBlock* block; uint16_t tableSelection = UINT16_MAX; };
struct Page {
  TextBlock block;
  bool forEachTextLine(bool (*fn)(const PageTextLine&, void*), void* context) const {
    return fn(PageTextLine{&block}, context);
  }
};
struct Section {
  std::vector<Page> pages;
  int currentPage = 0;
  int pageCount = 0;
  int reads = 0;
  bool failRead = false;
  Section(std::initializer_list<Page> values) : pages(values), pageCount(pages.size()) {}
  std::unique_ptr<Page> loadPage(int index) {
    ++reads;
    return failRead ? nullptr : std::make_unique<Page>(pages.at(index));
  }
};
'''

CASES = r'''
int main() {
  ClippingPageMatch match;
  // #720: the final word matches the beginning of a clipping elsewhere.
  Section unrelated{{{{"out,", "grasping", "it", "in", "his", "teeth,", "and"}}},
                    {{{"something", "unrelated"}}}};
  assert(!findClippingTextOnPage(unrelated, unrelated.pages[0], "and then he left", match));
  assert(unrelated.currentPage == 0);

  // A real clipping reflowed into 2+2 words must highlight both halves.
  Section split{{{{"prefix", "one", "two"}}}, {{{"three", "four", "suffix"}}}};
  assert(findClippingTextOnPage(split, split.pages[0], "one two three four", match));
  assert(match.startWord == 1 && match.endWord == 2);
  assert(match.startsAtClipStart && !match.reachesClipEnd);
  split.currentPage = 1;
  assert(findClippingTextOnPage(split, split.pages[1], "one two three four", match));
  assert(match.startWord == 0 && match.endWord == 1);
  assert(!match.startsAtClipStart && match.reachesClipEnd);
  assert(split.currentPage == 1);

  // Also preserve two-word clippings split 1+1, including a split token.
  Section shortSplit{{{{"one"}}}, {{{"two"}}}};
  assert(findClippingTextOnPage(shortSplit, shortSplit.pages[0], "one two", match));
  shortSplit.currentPage = 1;
  assert(findClippingTextOnPage(shortSplit, shortSplit.pages[1], "one two", match));
  Section hyphen{{{{"one", "cor-"}}}, {{{"rectly", "done"}}}};
  assert(findClippingTextOnPage(hyphen, hyphen.pages[0], "one correctly done", match));
  assert(match.startWord == 0 && match.endWord == 1);

  // An unrelated leading clipping suffix must not match either.
  Section tail{{{{"something", "unrelated"}}}, {{{"four", "suffix"}}}};
  tail.currentPage = 1;
  assert(!findClippingTextOnPage(tail, tail.pages[1], "one two three four", match));

  // Complete short clips and pages with no candidates need no neighbor reads.
  Section complete{{{{"prefix", "one", "suffix"}}}, {{{"other"}}}};
  assert(findClippingTextOnPage(complete, complete.pages[0], "one", match));
  assert(match.startWord == 1 && match.endWord == 1 && complete.reads == 0);
  assert(!findClippingTextOnPage(complete, complete.pages[0], "no matching text", match));
  assert(complete.reads == 0);

  // Missing adjacent cache data must not make an unverified fragment visible.
  split.currentPage = 0;
  split.failRead = true;
  assert(!findClippingTextOnPage(split, split.pages[0], "one two three four", match));
}
'''


def main():
    source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
    start = source.index("bool hasEmSpacePrefix(const char*")
    end = source.index("bool findClippingStoredRangeOnPage(", start)
    with tempfile.TemporaryDirectory(prefix="crossink-clipping-matcher-") as directory:
        source_path = Path(directory) / "matcher.cpp"
        binary = Path(directory) / "matcher"
        source_path.write_text(FIXTURES + source[start:end] + CASES)
        subprocess.run(["c++", "-std=c++17", "-I" + str(ROOT / "src"),
                        str(source_path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: clipping page boundaries, reflow, short clips, and failed neighbor reads")


if __name__ == "__main__":
    main()
