#pragma once
#include <cstddef>
#include <cstdint>

// What a store's exchange history lets a sync tell BookOrbit. Both the highlight and the
// bookmark exchanges, and both their stores, depend on this, so it stands apart from either's
// wire types.

/**
 * What the device says about its own copy of a book's highlights (or bookmarks) on the first
 * request of an exchange, BookOrbit's `sidecarState`.
 *
 * Since BookOrbit 3.5.0 a complete key set that keeps none of the rows this copy had applied (two
 * at least) reads as a lost copy -- a re-download, a wiped card: the server deletes nothing and
 * offers every row again. That guess cannot tell a lost copy from a reader who deleted every
 * synced highlight of a book on purpose, and would hand those back. Known is how a device says
 * its key set is the truth, deletions included; New asks for everything again, deleting nothing.
 * Unstated leaves the server to guess, and is the only choice against a server that predates the
 * field: it rejects a request that carries it.
 */
enum class BookOrbitSidecarState : uint8_t { Unstated, Known, New };

/**
 * The watermark a store is given by its first exchange when nothing it sent moved it further.
 * 1 is 1970-01-01 00:00:01: no highlight or bookmark is ever dated that early, so it hides none,
 * and a firmware that does not read it this way sees an ordinary watermark.
 */
inline constexpr uint32_t BOOKORBIT_EXCHANGED_WATERMARK = 1;

/**
 * The state one store may report.
 *
 * Known needs proof that this very store has been through an exchange, which is what its
 * watermark records (see BookOrbitAnnotationStore::markExchanged): a store lost and started over
 * by a new highlight is readable again, but holds only that highlight, and reporting its key set
 * as the truth would delete everything else the server had for this copy.
 *
 * @param storeReadable The store's file exists and parsed
 * @param watermark The store's watermark, 0 until it has been through an exchange
 * @param localCount Highlights (or bookmarks) held locally, with or without a position
 * @param serverAccepts The server advertises the sidecarState capability
 */
inline BookOrbitSidecarState bookOrbitSidecarState(const bool storeReadable, const uint32_t watermark,
                                                   const size_t localCount, const bool serverAccepts) {
  if (!serverAccepts) return BookOrbitSidecarState::Unstated;
  if (storeReadable) return watermark != 0 ? BookOrbitSidecarState::Known : BookOrbitSidecarState::Unstated;
  // No store and highlights held anyway (made before an account was set up, or the store alone
  // was lost): their key set cannot be built, and "new" would have the server send each back.
  return localCount == 0 ? BookOrbitSidecarState::New : BookOrbitSidecarState::Unstated;
}
