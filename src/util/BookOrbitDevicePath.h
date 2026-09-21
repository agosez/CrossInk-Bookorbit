#pragma once

#include <cstddef>
#include <string>

/**
 * Turns the `devicePath` BookOrbit returns for a catalog file into a relative SD
 * path the catalog downloads into.
 *
 * BookOrbit renders the account's KOReader file naming template (or this device's
 * override) server-side and ships the result with the book detail, so the firmware
 * needs no template engine: it only has to make the rendered path safe to write.
 * The server already sanitizes each segment for cross-platform filesystems, but the
 * value arrives over the network and becomes a path this firmware creates, so it is
 * re-validated here — traversal segments dropped, every segment through the app's
 * own filename sanitizer, nesting depth and total length bounded.
 */
namespace BookOrbitDevicePath {

// Bytes allowed for the whole relative path, and the only bound on how deep a
// template may nest: folders are shed, deepest first, until the path fits. SdFat
// opens by full path, so this leaves room for the configured download folder inside
// a workable path budget.
constexpr size_t MAX_RELATIVE_PATH_BYTES = 180;

/**
 * Returns the sanitized relative path, without a leading slash
 * ("Sprawl/Sprawl - 01 - William Gibson - Neuromancer.epub"), or an empty string
 * when nothing usable remains — the caller then falls back to its own naming.
 */
std::string sanitizeRelativePath(const std::string& devicePath);

}  // namespace BookOrbitDevicePath
