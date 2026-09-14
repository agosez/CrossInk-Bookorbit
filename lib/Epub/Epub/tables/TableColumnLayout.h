#pragma once

#include <algorithm>
#include <cstdint>

// Dense eight-column tables commonly use the first column as a row label and
// the remaining columns for short values. Give that label column two shares so
// its wrapped text remains legible, while keeping all other table geometries
// unchanged.
namespace TableColumnLayout {

constexpr bool usesWideLeadingColumn(const uint8_t columnCount) { return columnCount == 8; }

constexpr uint8_t totalUnits(const uint8_t columnCount) {
  return static_cast<uint8_t>(columnCount + (usesWideLeadingColumn(columnCount) ? 1 : 0));
}

inline uint16_t columnStart(const uint16_t tableWidth, const uint8_t columnCount, const uint8_t columnIndex) {
  if (columnCount == 0) return 0;

  const uint8_t boundedIndex = std::min(columnIndex, columnCount);
  const uint8_t unitsBefore =
      static_cast<uint8_t>(boundedIndex + (usesWideLeadingColumn(columnCount) && boundedIndex > 0 ? 1 : 0));
  return static_cast<uint16_t>(static_cast<uint32_t>(tableWidth) * unitsBefore / totalUnits(columnCount));
}

inline uint16_t columnWidth(const uint16_t tableWidth, const uint8_t columnCount, const uint8_t startColumn,
                            const uint8_t span) {
  if (startColumn >= columnCount || span == 0) return 0;
  const uint8_t endColumn = std::min<uint8_t>(columnCount, static_cast<uint8_t>(startColumn + span));
  return static_cast<uint16_t>(columnStart(tableWidth, columnCount, endColumn) -
                               columnStart(tableWidth, columnCount, startColumn));
}

inline uint16_t innerWidth(const uint16_t tableWidth, const uint8_t columnCount, const uint8_t startColumn,
                           const uint8_t span, const uint8_t cellPadding) {
  const uint16_t width = columnWidth(tableWidth, columnCount, startColumn, span);
  return width > cellPadding * 2 ? static_cast<uint16_t>(width - cellPadding * 2) : 0;
}

}  // namespace TableColumnLayout
