#pragma once
/**
 * Building blocks shared by the xpath resolvers in this library: element-path bookkeeping over
 * an expat parse, and the KOReader-compatible path builders. Internal to lib/EpubPosition.
 */
#include <expat.h>

#include <cstring>
#include <string>
#include <vector>

inline std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

inline std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path,
                                       const int textNodeIndex, const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0 && charOffset > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

// Like buildParagraphXPath, but always emits the text-node offset, including offset 0: a
// highlight starting on a paragraph's first character still needs a range the server can
// resolve, and the paragraph-level form it would otherwise degrade to spans nothing.
inline std::string buildTextXPointer(const int spineIndex, const std::vector<PathSegment>& path,
                                     const int textNodeIndex, const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  xpath += "/text()[" + std::to_string(textNodeIndex > 0 ? textNodeIndex : 1) + "]." + std::to_string(charOffset);
  return xpath;
}

inline bool isNonVisibleTextTag(const std::string& name) {
  return name == "head" || name == "style" || name == "script" || name == "title" || name == "rp" || name == "rt";
}
