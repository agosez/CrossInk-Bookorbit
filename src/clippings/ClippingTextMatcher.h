#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ClippingTextMatcher {

enum class TokenFragmentMatch : uint8_t {
  MISMATCH,
  CONTINUES_TOKEN,
  COMPLETES_TOKEN,
};

struct TokenFragmentResult {
  TokenFragmentMatch match = TokenFragmentMatch::MISMATCH;
  size_t tokenBytes = 0;
};

inline bool isNonBreakingSpace(const char* text, const size_t length, const size_t offset) {
  return offset + 1 < length && static_cast<unsigned char>(text[offset]) == 0xC2 &&
         static_cast<unsigned char>(text[offset + 1]) == 0xA0;
}

// Compares one rendered word fragment with a saved clipping token. Layout can
// split one logical token into adjacent display fragments (for example, a word
// followed by an ellipsis when focus reading is enabled). A non-breaking space
// before the next display fragment is equivalent to the ordinary space stored
// in the clipping. An inserted hyphen is omitted from the saved token before
// matching its fragment.
inline TokenFragmentResult matchTokenFragmentWithLength(const char* word, const bool endsWithInsertedHyphen,
                                                        const char* token, const size_t tokenLen,
                                                        const size_t tokenOffset) {
  if (!word || !token || tokenLen == 0 || tokenOffset >= tokenLen) {
    return {};
  }

  size_t wordLen = std::strlen(word);
  if (endsWithInsertedHyphen) {
    if (wordLen == 0 || word[wordLen - 1] != '-') {
      return {};
    }
    wordLen--;
  }

  size_t wordOffset = 0;
  size_t matchedTokenBytes = 0;
  while (wordOffset < wordLen && tokenOffset + matchedTokenBytes < tokenLen) {
    if (isNonBreakingSpace(word, wordLen, wordOffset) && token[tokenOffset + matchedTokenBytes] == ' ') {
      wordOffset += 2;
      matchedTokenBytes++;
      continue;
    }
    if (word[wordOffset] != token[tokenOffset + matchedTokenBytes]) {
      return {};
    }
    wordOffset++;
    matchedTokenBytes++;
  }

  if (wordOffset != wordLen) {
    return {};
  }

  const bool completesToken = tokenOffset + matchedTokenBytes == tokenLen;
  if (completesToken && endsWithInsertedHyphen) {
    return {};
  }
  return {completesToken ? TokenFragmentMatch::COMPLETES_TOKEN : TokenFragmentMatch::CONTINUES_TOKEN,
          matchedTokenBytes};
}

inline TokenFragmentMatch matchTokenFragment(const char* word, const bool endsWithInsertedHyphen, const char* token,
                                             const size_t tokenLen, const size_t tokenOffset) {
  return matchTokenFragmentWithLength(word, endsWithInsertedHyphen, token, tokenLen, tokenOffset).match;
}

}  // namespace ClippingTextMatcher
