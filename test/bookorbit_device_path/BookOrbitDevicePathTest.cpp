// Covers the sanitizer that turns BookOrbit's rendered devicePath (the account's
// KOReader file naming template, applied server-side) into a path this firmware is
// willing to create on the SD card.

#include <gtest/gtest.h>

#include <string>

#include "util/BookOrbitDevicePath.h"

namespace {

using BookOrbitDevicePath::sanitizeRelativePath;

std::string repeat(const char c, const size_t count) { return std::string(count, c); }

TEST(BookOrbitDevicePath, KeepsAFlatName) {
  EXPECT_EQ(sanitizeRelativePath("William Gibson - Neuromancer.epub"), "William Gibson - Neuromancer.epub");
}

TEST(BookOrbitDevicePath, KeepsTheTemplatesFolders) {
  EXPECT_EQ(sanitizeRelativePath("Sprawl/Sprawl - 01 - William Gibson - Neuromancer.epub"),
            "Sprawl/Sprawl - 01 - William Gibson - Neuromancer.epub");
}

TEST(BookOrbitDevicePath, DropsALeadingSlashSoThePathStaysRelative) {
  EXPECT_EQ(sanitizeRelativePath("/Series/Sprawl/Neuromancer.epub"), "Series/Sprawl/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, TreatsBackslashesAsSeparators) {
  EXPECT_EQ(sanitizeRelativePath("Series\\Sprawl\\Neuromancer.epub"), "Series/Sprawl/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, DropsTraversalSegments) {
  EXPECT_EQ(sanitizeRelativePath("../../Neuromancer.epub"), "Neuromancer.epub");
  EXPECT_EQ(sanitizeRelativePath("Series/../../../Neuromancer.epub"), "Series/Neuromancer.epub");
  EXPECT_EQ(sanitizeRelativePath("./Series/./Neuromancer.epub"), "Series/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, DropsEmptyAndBlankSegments) {
  EXPECT_EQ(sanitizeRelativePath("Series//   //Neuromancer.epub"), "Series/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, RejectsAPathWithNothingUsableLeft) {
  EXPECT_EQ(sanitizeRelativePath(""), "");
  EXPECT_EQ(sanitizeRelativePath("/"), "");
  EXPECT_EQ(sanitizeRelativePath("../.."), "");
  EXPECT_EQ(sanitizeRelativePath("   /  "), "");
}

TEST(BookOrbitDevicePath, AppendsTheExtensionWhenTheTemplateOmitsIt) {
  EXPECT_EQ(sanitizeRelativePath("Sprawl/Neuromancer"), "Sprawl/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, DoesNotDoubleAnExtensionItAlreadyHas) {
  EXPECT_EQ(sanitizeRelativePath("Neuromancer.EPUB"), "Neuromancer.EPUB");
}

TEST(BookOrbitDevicePath, ReplacesCharactersTheFilesystemRejects) {
  EXPECT_EQ(sanitizeRelativePath("Sci-Fi: Best of/Gibson? Neuromancer.epub"),
            "Sci-Fi_ Best of/Gibson_ Neuromancer.epub");
}

TEST(BookOrbitDevicePath, KeepsNonAsciiTitles) {
  EXPECT_EQ(sanitizeRelativePath("Séries/Léa Silhol - Avant l'Hiver.epub"), "Séries/Léa Silhol - Avant l'Hiver.epub");
}

TEST(BookOrbitDevicePath, KeepsEveryFolderAnUnusualTemplateAsksFor) {
  // Nesting is bounded by the path budget alone: the catalog lists its downloads from
  // the index, which records where each one went, so depth costs nothing to find.
  EXPECT_EQ(sanitizeRelativePath("a/b/c/d/e/Neuromancer.epub"), "a/b/c/d/e/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, ShedsFoldersUntilThePathFits) {
  const std::string deep = repeat('d', 100);
  const std::string wide = repeat('w', 100);
  // 100 + 1 + 100 + 1 + 16 is past the budget; the deepest folder goes first.
  EXPECT_EQ(sanitizeRelativePath(deep + "/" + wide + "/Neuromancer.epub"), deep + "/Neuromancer.epub");
}

TEST(BookOrbitDevicePath, KeepsTheFileNameWhenEveryFolderHadToGo) {
  const std::string huge = repeat('x', 140);
  const std::string longName = repeat('n', 60) + ".epub";
  const std::string result = sanitizeRelativePath(huge + "/" + huge + "/" + longName);
  EXPECT_LE(result.size(), BookOrbitDevicePath::MAX_RELATIVE_PATH_BYTES);
  EXPECT_EQ(result, longName);
}

TEST(BookOrbitDevicePath, TruncatesAnOverlongSegmentInsteadOfDroppingIt) {
  const std::string result = sanitizeRelativePath(repeat('x', 400) + ".epub");
  EXPECT_LE(result.size(), BookOrbitDevicePath::MAX_RELATIVE_PATH_BYTES);
  EXPECT_NE(result.find(".epub"), std::string::npos);
}

}  // namespace
