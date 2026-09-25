#pragma once

#include <cstddef>
#include <cstdint>

enum class DictionaryWordPartSeparator : uint8_t { None = 0, Hyphen, EnDash, EmDash };

struct DictionaryWordPart {
  const char* text = nullptr;
  size_t length = 0;
  size_t sourceOffset = 0;
  DictionaryWordPartSeparator separatorBefore = DictionaryWordPartSeparator::None;
};

// A trailing ASCII hyphen is the layout marker used to reconnect a word split
// across lines (for example "exter-" + "nity"). Only a single internal
// ASCII hyphen splits a compound; preserving adjacent hyphens avoids creating
// a punctuation-only selectable part. Retain the existing en/em-dash behavior
// for compound words.
inline DictionaryWordPartSeparator dictionaryWordPartSeparatorAt(const char* text, const size_t length,
                                                                 const size_t offset) {
  if (!text || offset >= length) return DictionaryWordPartSeparator::None;
  if (text[offset] == '-') {
    return offset > 0 && text[offset - 1] != '-' && offset + 1 < length && text[offset + 1] != '-'
               ? DictionaryWordPartSeparator::Hyphen
               : DictionaryWordPartSeparator::None;
  }
  if (offset + 2 >= length || static_cast<uint8_t>(text[offset]) != 0xE2 ||
      static_cast<uint8_t>(text[offset + 1]) != 0x80) {
    return DictionaryWordPartSeparator::None;
  }
  if (static_cast<uint8_t>(text[offset + 2]) == 0x93) return DictionaryWordPartSeparator::EnDash;
  if (static_cast<uint8_t>(text[offset + 2]) == 0x94) return DictionaryWordPartSeparator::EmDash;
  return DictionaryWordPartSeparator::None;
}

inline size_t dictionaryWordPartSeparatorLength(const DictionaryWordPartSeparator separator) {
  switch (separator) {
    case DictionaryWordPartSeparator::Hyphen:
      return 1;
    case DictionaryWordPartSeparator::EnDash:
    case DictionaryWordPartSeparator::EmDash:
      return 3;
    case DictionaryWordPartSeparator::None:
      return 0;
  }
  return 0;
}

inline const char* dictionaryWordPartSeparatorText(const DictionaryWordPartSeparator separator) {
  switch (separator) {
    case DictionaryWordPartSeparator::Hyphen:
      return "-";
    case DictionaryWordPartSeparator::EnDash:
      return "\xE2\x80\x93";
    case DictionaryWordPartSeparator::EmDash:
      return "\xE2\x80\x94";
    case DictionaryWordPartSeparator::None:
      return "";
  }
  return "";
}

inline int dictionaryWordPartVisualOffset(const int wordWidth, const int logicalPrefixWidth, const int partWidth,
                                          const bool isRtl) {
  // GfxRenderer draws the bidi-reordered visual text left-to-right. The
  // logical prefix is therefore on the visual right of an RTL compound.
  return isRtl ? wordWidth - logicalPrefixWidth - partWidth : logicalPrefixWidth;
}

template <typename Sink>
void forEachDictionaryWordPart(const char* text, const size_t length, Sink&& sink) {
  if (!text) return;
  size_t partStart = 0;
  DictionaryWordPartSeparator separatorBefore = DictionaryWordPartSeparator::None;
  for (size_t i = 0; i < length;) {
    const DictionaryWordPartSeparator separator = dictionaryWordPartSeparatorAt(text, length, i);
    const size_t separatorLength = dictionaryWordPartSeparatorLength(separator);
    if (separatorLength == 0) {
      ++i;
      continue;
    }
    if (i > partStart) sink(DictionaryWordPart{text + partStart, i - partStart, partStart, separatorBefore});
    i += separatorLength;
    partStart = i;
    separatorBefore = separator;
  }
  if (partStart < length) {
    sink(DictionaryWordPart{text + partStart, length - partStart, partStart, separatorBefore});
  }
}
