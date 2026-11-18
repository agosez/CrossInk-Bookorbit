#pragma once

#include <memory>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

class Epub;
class GfxRenderer;

namespace BookOrbitRemotePosition {

/**
 * The server's position in local pages: ProgressMapper's estimate, refined through the
 * section cache's LUTs (list item, anchor, then paragraph) when that chapter is cached.
 * The current chapter's page count lets the mapper estimate the density of the others.
 */
CrossPointPosition toLocal(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const KOReaderProgress& remote,
                           int currentSpineIndex, int totalPagesInSpine);

/**
 * The position's progress through the book in the local model.
 *
 * Percentages from different engines are not comparable: KOReader's model runs up to half
 * a point ahead of ours on some books, which made a remote position read as "further"
 * even after reading past it here. Everything that orders the two positions -- the smart
 * decision, the default selection, the percentage the screen shows for the remote side --
 * therefore uses the remote position mapped into local pages.
 */
float localPercent(const Epub& epub, const CrossPointPosition& position);

}  // namespace BookOrbitRemotePosition
