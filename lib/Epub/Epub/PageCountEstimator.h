#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

// Page-count estimates use fixed-point page units so image pages can be kept out
// of the byte-density projection.  One physical page is 256 units.
struct PageCountEstimator {
  static constexpr uint32_t kUnitsPerPage = 256;
  static constexpr uint32_t kMaxPages = 60000;

  static constexpr uint64_t saturatingMultiply(const uint64_t lhs, const uint64_t rhs) {
    if (lhs == 0 || rhs == 0) return 0;
    if (lhs > std::numeric_limits<uint64_t>::max() / rhs) return std::numeric_limits<uint64_t>::max();
    return lhs * rhs;
  }

  // Return the non-image portion of the completed physical pages.  Image units
  // are capped at the available physical-page budget before subtraction so a
  // malformed caller cannot underflow the fixed-point arithmetic.
  static constexpr uint64_t nonImageUnits(const uint32_t physicalPages, const uint64_t imageUnits) {
    const uint64_t physicalUnits = static_cast<uint64_t>(physicalPages) * kUnitsPerPage;
    return physicalUnits > std::min(imageUnits, physicalUnits) ? physicalUnits - std::min(imageUnits, physicalUnits)
                                                               : 0;
  }

  // Project only non-image units through the XHTML byte ratio.  A zero or
  // already-complete input deliberately leaves the known units unchanged.
  static constexpr uint64_t projectNonImageUnits(const uint64_t knownNonImageUnits, const uint32_t totalBytes,
                                                 const uint32_t consumedBytes) {
    if (knownNonImageUnits == 0 || consumedBytes == 0 || totalBytes <= consumedBytes) {
      return knownNonImageUnits;
    }
    if (knownNonImageUnits > std::numeric_limits<uint64_t>::max() / totalBytes) {
      return std::numeric_limits<uint64_t>::max();
    }
    return saturatingMultiply(knownNonImageUnits, totalBytes) / consumedBytes;
  }

  // Estimate total pages from completed physical pages and their protected
  // image units.  This is allocation-free and preserves the old byte-ratio
  // result exactly when imageUnits is zero.
  static constexpr uint32_t estimate(const uint32_t physicalPages, const uint64_t imageUnits, const uint32_t totalBytes,
                                     const uint32_t consumedBytes) {
    const uint64_t knownImageUnits = std::min(imageUnits, static_cast<uint64_t>(physicalPages) * kUnitsPerPage);
    const uint64_t knownNonImage = nonImageUnits(physicalPages, knownImageUnits);
    const uint64_t projectedUnits = knownImageUnits + projectNonImageUnits(knownNonImage, totalBytes, consumedBytes);
    const uint64_t projectedPages = projectedUnits / kUnitsPerPage;
    const uint64_t minimumPages = physicalPages;
    const uint64_t bounded = std::max(minimumPages, projectedPages);
    return static_cast<uint32_t>(std::min<uint64_t>(kMaxPages, bounded));
  }
};
