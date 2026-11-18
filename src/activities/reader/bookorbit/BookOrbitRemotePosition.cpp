#include "BookOrbitRemotePosition.h"

#include <Epub.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "Epub/Section.h"

namespace BookOrbitRemotePosition {

CrossPointPosition toLocal(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const KOReaderProgress& remote,
                           const int currentSpineIndex, const int totalPagesInSpine) {
  KOReaderPosition koPos = {remote.progress, remote.percentage};
  CrossPointPosition remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  if (remotePosition.hasLiIndex || remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("BookOrbit", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("BookOrbit", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("BookOrbit", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("BookOrbit", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        LOG_DBG("BookOrbit", "Paragraph %u -> LUT page %d, intra page %d, using %d", remotePosition.paragraphIndex,
                *paragraphPage, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("BookOrbit", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
  }
  return remotePosition;
}

float localPercent(const Epub& epub, const CrossPointPosition& position) {
  const float intra = position.totalPages > 1
                          ? static_cast<float>(position.pageNumber) / static_cast<float>(position.totalPages - 1)
                          : 0.0f;
  return epub.calculateProgress(position.spineIndex, intra);
}

}  // namespace BookOrbitRemotePosition
