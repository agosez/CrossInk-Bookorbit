#include "HighlightPositionResolver.h"

#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "XPathBuildCommon.h"

namespace {
bool isXmlSpace(const unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Byte length of a Unicode space at `p`, or 0. Mirrors the JavaScript \s class the server
// collapses with: French EPUBs put non-breaking (often narrow) spaces before punctuation, and
// a collapse that does not treat those as whitespace diverges from the server's on exactly
// those passages.
size_t utf8SpaceLen(const unsigned char* p, const unsigned char* end) {
  if (p >= end) return 0;
  if (p[0] == 0xC2 && p + 1 < end && p[1] == 0xA0) return 2;                  // U+00A0
  if (p[0] == 0xE1 && p + 2 < end && p[1] == 0x9A && p[2] == 0x80) return 3;  // U+1680
  if (p[0] == 0xE2 && p + 2 < end && p[1] == 0x80) {
    const unsigned char c2 = p[2];
    if ((c2 >= 0x80 && c2 <= 0x8A) || c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF)
      return 3;  // U+2000-200A, 2028, 2029, 202F
  }
  if (p[0] == 0xE2 && p + 2 < end && p[1] == 0x81 && p[2] == 0x9F) return 3;  // U+205F
  if (p[0] == 0xE3 && p + 2 < end && p[1] == 0x80 && p[2] == 0x80) return 3;  // U+3000
  if (p[0] == 0xEF && p + 2 < end && p[1] == 0xBB && p[2] == 0xBF) return 3;  // U+FEFF
  return 0;
}

/**
 * Streams a chapter through the crengine-style whitespace collapse and hands every visible
 * codepoint of <p> content to a consumer, with the position data an xpointer needs.
 *
 * Both resolution passes (locate, then extract) must see byte-identical streams, so the
 * collapse lives here once: whitespace runs become one space, leading whitespace after a
 * block boundary drops, inline elements do not interrupt the flow, and paragraph breaks
 * arrive as a separator that belongs to no node -- the exact model the server resolves
 * xpointer offsets against. Soft hyphens pass through as ordinary codepoints (the server
 * keeps them too); consumers that must ignore them do so themselves.
 *
 * Memory is deliberately flat: nothing of the chapter is retained beyond the current path,
 * which is what lets this run at highlight-creation time on a heap the reader has already
 * mostly spoken for. An earlier version captured whole paragraphs and their mapping tables
 * first (~40 KB for a long window) and took the device down with allocation aborts.
 */
class CollapsedParagraphStream final : public Print {
 public:
  class Consumer {
   public:
    virtual ~Consumer() = default;
    // One collapsed codepoint of paragraph text. `path`/`textNodeIndex`/`offsetInNode` locate
    // it for buildTextXPointer; `paragraphOrdinal` is the running <p> count. Return false to
    // stop the parse.
    virtual bool onCodepoint(const char* bytes, size_t byteLength, const std::vector<PathSegment>& path,
                             int textNodeIndex, uint16_t offsetInNode, int paragraphOrdinal) = 0;
    // A paragraph boundary between two visible codepoints (belongs to no node).
    virtual void onParagraphBreak() = 0;
  };

  CollapsedParagraphStream(Consumer& consumer, const char* targetTag) : consumer(consumer), targetTag(targetTag) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &CollapsedParagraphStream::startElement, &CollapsedParagraphStream::endElement);
    XML_SetCharacterDataHandler(parser, &CollapsedParagraphStream::characterData);
  }

  ~CollapsedParagraphStream() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }
    return size;
  }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<CollapsedParagraphStream*>(userData)->onStartElement(name);
  }
  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<CollapsedParagraphStream*>(userData)->onEndElement(name);
  }
  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<CollapsedParagraphStream*>(userData)->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
        textNodeCounts.push_back(0);
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeCounts.push_back(0);
    atNodeBoundary = true;

    if (name == targetTag) {
      paragraphOrdinal++;
      inParagraphDepth = depth;
      inParagraph = true;
      atBlockBoundary = true;
      lastWasSpace = false;
      if (emittedAnything) pendingParagraphBreak = true;
    }
    if (isNonVisibleTextTag(name)) nonVisibleDepth++;
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    depth--;
    if (!insideBody) {
      return;
    }
    if (isNonVisibleTextTag(name) && nonVisibleDepth > 0) nonVisibleDepth--;
    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      textNodeCounts.clear();
      path.clear();
      return;
    }
    if (inParagraph && depth == inParagraphDepth && name == targetTag) {
      inParagraph = false;
    }
    if (!path.empty()) path.pop_back();
    if (!parentStates.empty()) parentStates.pop_back();
    if (!textNodeCounts.empty()) textNodeCounts.pop_back();
    atNodeBoundary = true;
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!inParagraph || nonVisibleDepth > 0 || len <= 0 || textNodeCounts.empty()) {
      return;
    }
    if (atNodeBoundary) {
      textNodeCounts.back()++;
      emittedInNode = 0;
      atNodeBoundary = false;
    }

    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
    const unsigned char* end = ptr + len;
    while (ptr < end && !stopped) {
      const unsigned char* charStart = ptr;
      utf8NextCodepoint(&ptr);
      const size_t byteLength = static_cast<size_t>(ptr - charStart);

      const bool isSpace = (byteLength == 1 && isXmlSpace(*charStart)) || utf8SpaceLen(charStart, end) == byteLength;
      if (isSpace) {
        if (!atBlockBoundary && !lastWasSpace) {
          emit(" ", 1);
          lastWasSpace = true;
        }
        continue;
      }
      emit(reinterpret_cast<const char*>(charStart), byteLength);
      atBlockBoundary = false;
      lastWasSpace = false;
    }
  }

  void emit(const char* bytes, const size_t byteLength) {
    if (pendingParagraphBreak) {
      consumer.onParagraphBreak();
      pendingParagraphBreak = false;
    }
    emittedAnything = true;
    if (!consumer.onCodepoint(bytes, byteLength, path, textNodeCounts.back(), emittedInNode, paragraphOrdinal)) {
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
      return;
    }
    emittedInNode++;
  }

  Consumer& consumer;
  XML_Parser parser = nullptr;
  const char* targetTag;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool inParagraph = false;
  bool atNodeBoundary = true;
  bool atBlockBoundary = true;
  bool lastWasSpace = false;
  bool pendingParagraphBreak = false;
  bool emittedAnything = false;
  int depth = 0;
  int bodyDepth = -1;
  int inParagraphDepth = -1;
  int nonVisibleDepth = 0;
  int paragraphOrdinal = 0;
  uint16_t emittedInNode = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::vector<int> textNodeCounts;
};

bool isSoftHyphen(const char* bytes, const size_t byteLength) {
  return byteLength == 2 && static_cast<unsigned char>(bytes[0]) == 0xC2 &&
         static_cast<unsigned char>(bytes[1]) == 0xAD;
}

/**
 * Pass 1: find the highlight's text in the chapter's collapsed stream.
 *
 * KMP over the byte stream with spaces and soft hyphens dropped from both sides -- the
 * clipping's text was rebuilt from rendered words, so its spacing cannot be trusted and the
 * rendered words consumed the source's soft hyphens. Costs the pattern, its prefix table and
 * a small ring of recent codepoint ordinals; nothing of the chapter is kept.
 *
 * Several occurrences are all recorded (a short highlight can repeat), and the caller picks
 * the one whose paragraph sits closest to the layout's paragraph hint.
 */
class HighlightLocatePass final : public CollapsedParagraphStream::Consumer {
 public:
  struct Match {
    size_t startOrdinal = 0;  // stripped-codepoint ordinal of the first matched codepoint
    size_t endOrdinal = 0;    // ordinal of the last matched codepoint (inclusive)
    int paragraphOrdinal = 0;
  };
  static constexpr size_t MAX_MATCHES = 6;

  explicit HighlightLocatePass(const std::string& strippedNeedle) : needle(strippedNeedle) {
    prefixTable.assign(needle.size(), 0);
    for (size_t i = 1; i < needle.size(); i++) {
      size_t k = prefixTable[i - 1];
      while (k > 0 && needle[i] != needle[k]) k = prefixTable[k - 1];
      if (needle[i] == needle[k]) k++;
      prefixTable[i] = static_cast<uint16_t>(k);
    }
    ordinalRing.assign(needle.size(), 0);
  }

  bool onCodepoint(const char* bytes, const size_t byteLength, const std::vector<PathSegment>&, int, uint16_t,
                   const int paragraphOrdinal) override {
    if (bytes[0] == ' ' || isSoftHyphen(bytes, byteLength)) return true;

    for (size_t i = 0; i < byteLength; i++) {
      ordinalRing[ringAt] = strippedOrdinal;
      ringAt = (ringAt + 1) % ordinalRing.size();
      const char c = bytes[i];
      while (matched > 0 && c != needle[matched]) matched = prefixTable[matched - 1];
      if (c == needle[matched]) matched++;
      if (matched == needle.size()) {
        Match match;
        // The ring slot now at `ringAt` holds the ordinal of the byte needle.size() back:
        // the first byte of this occurrence.
        match.startOrdinal = ordinalRing[ringAt];
        match.endOrdinal = strippedOrdinal;
        match.paragraphOrdinal = paragraphOrdinal;
        if (matches.size() < MAX_MATCHES) matches.push_back(match);
        matched = prefixTable[matched - 1];
      }
    }
    strippedOrdinal++;
    return true;
  }

  void onParagraphBreak() override {}

  const std::vector<Match>& getMatches() const { return matches; }

 private:
  const std::string& needle;
  std::vector<uint16_t> prefixTable;
  std::vector<size_t> ordinalRing;  // stripped ordinal of each of the last needle-size bytes
  size_t ringAt = 0;
  size_t matched = 0;
  size_t strippedOrdinal = 0;
  std::vector<Match> matches;
};

/**
 * Pass 2: walk the same stream again and, at the located ordinals, take what pass 1 could
 * not afford to keep: the xpointer coordinates of both edges and the exact source text in
 * between (paragraph breaks as newlines, spaces and soft hyphens preserved -- that text is
 * what the server reads back at these positions). Stops as soon as the end is captured.
 */
class HighlightExtractPass final : public CollapsedParagraphStream::Consumer {
 public:
  HighlightExtractPass(const size_t startOrdinal, const size_t endOrdinal, const int spineIndex,
                       const size_t sourceTextCap)
      : startOrdinal(startOrdinal), endOrdinal(endOrdinal), spineIndex(spineIndex), sourceTextCap(sourceTextCap) {}

  bool onCodepoint(const char* bytes, const size_t byteLength, const std::vector<PathSegment>& path,
                   const int textNodeIndex, const uint16_t offsetInNode, int) override {
    const bool counts = !(bytes[0] == ' ' || isSoftHyphen(bytes, byteLength));

    if (inSpan && sourceText.size() + byteLength <= sourceTextCap) {
      if (pendingBreak) {
        sourceText.push_back('\n');
        pendingBreak = false;
      }
      sourceText.append(bytes, byteLength);
    }

    if (!counts) return true;

    if (strippedOrdinal == startOrdinal) {
      pos0 = buildTextXPointer(spineIndex, path, textNodeIndex, offsetInNode);
      inSpan = true;
      pendingBreak = false;
      sourceText.append(bytes, byteLength);
    }
    if (strippedOrdinal == endOrdinal) {
      pos1 = buildTextXPointer(spineIndex, path, textNodeIndex, offsetInNode + 1);
      return false;  // everything needed is in hand
    }
    strippedOrdinal++;
    return true;
  }

  void onParagraphBreak() override {
    if (inSpan) pendingBreak = true;
  }

  bool complete() const { return !pos0.empty() && !pos1.empty(); }

  std::string pos0;
  std::string pos1;
  std::string sourceText;

 private:
  const size_t startOrdinal;
  const size_t endOrdinal;
  const int spineIndex;
  const size_t sourceTextCap;
  size_t strippedOrdinal = 0;
  bool inSpan = false;
  bool pendingBreak = false;
};
}  // namespace

bool HighlightPositionResolver::findHighlightXPointers(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const uint16_t paragraphIndex, const std::string& highlightText,
                                                       std::string& outPos0, std::string& outPos1,
                                                       std::string* outSourceText) {
  outPos0.clear();
  outPos1.clear();
  if (outSourceText) outSourceText->clear();
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount() || paragraphIndex == 0 ||
      paragraphIndex == UINT16_MAX || highlightText.empty()) {
    return false;
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) return false;

  // The pattern: the clipping's text with whitespace and soft hyphens dropped. Its spacing was
  // reconstructed geometrically from rendered words and cannot be trusted, and the rendered
  // words consumed the soft hyphens the source still carries.
  std::string needle;
  needle.reserve(highlightText.size());
  {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(highlightText.data());
    const unsigned char* end = ptr + highlightText.size();
    while (ptr < end) {
      if (*ptr == 0xC2 && ptr + 1 < end && static_cast<unsigned char>(ptr[1]) == 0xAD) {
        ptr += 2;
        continue;
      }
      const size_t spaceLen = utf8SpaceLen(ptr, end);
      if (spaceLen > 0) {
        ptr += spaceLen;
        continue;
      }
      if (isXmlSpace(*ptr)) {
        ptr++;
        continue;
      }
      needle.push_back(static_cast<char>(*ptr++));
    }
  }
  if (needle.empty()) return false;

  HighlightLocatePass locate(needle);
  {
    CollapsedParagraphStream stream(locate, "p");
    if (!stream.ok() || !epub->readItemContentsToStream(href, stream, 1024) || !stream.finish()) {
      return false;
    }
  }
  if (locate.getMatches().empty()) {
    LOG_ERR("KOX", "Highlight text not found in spine %d (hint p%u): \"%.24s\"", spineIndex, paragraphIndex,
            needle.c_str());
    return false;
  }

  // A short highlight can occur several times; the layout's paragraph hint, however skewed its
  // counting, still points near the right one.
  const HighlightLocatePass::Match* best = &locate.getMatches().front();
  for (const auto& match : locate.getMatches()) {
    const int bestDistance = std::abs(best->paragraphOrdinal - static_cast<int>(paragraphIndex));
    const int distance = std::abs(match.paragraphOrdinal - static_cast<int>(paragraphIndex));
    if (distance < bestDistance) best = &match;
  }

  // Source text runs a little longer than the stripped pattern (spaces, soft hyphens); the
  // clipping store caps what it keeps anyway.
  HighlightExtractPass extract(best->startOrdinal, best->endOrdinal, spineIndex, highlightText.size() + 512);
  {
    CollapsedParagraphStream stream(extract, "p");
    if (!stream.ok() || !epub->readItemContentsToStream(href, stream, 1024)) {
      return false;
    }
    stream.finish();
  }
  if (!extract.complete()) return false;

  outPos0 = std::move(extract.pos0);
  outPos1 = std::move(extract.pos1);
  if (outSourceText) *outSourceText = std::move(extract.sourceText);
  return true;
}
