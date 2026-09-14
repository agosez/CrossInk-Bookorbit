#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <string_view>

struct FontFamilyPointSizeRange {
  uint8_t first = 0;
  uint8_t last = 0;

  bool isValid() const { return first != 0; }
};

inline FontFamilyPointSizeRange fontFamilyPointSizeRange(const SdCardFontFamilyInfo& family) {
  FontFamilyPointSizeRange range;
  for (const auto& file : family.files) {
    if (file.style != 0) continue;
    if (!range.isValid() || file.pointSize < range.first) range.first = file.pointSize;
    if (file.pointSize > range.last) range.last = file.pointSize;
  }
  return range;
}

inline std::string fontFamilyLabel(const std::string_view familyName, const FontFamilyPointSizeRange range) {
  std::string label;
  label.reserve(familyName.size() + 24);
  label.append(familyName.data(), familyName.size());
  if (!range.isValid()) return label;

  label += " (";
  label += std::to_string(range.first);
  if (range.last != range.first) {
    label += "-";
    label += std::to_string(range.last);
  }
  label += "pt";
  label += ")";
  return label;
}
