#include "EpubGrayscale.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>

#include <algorithm>

namespace EpubGrayscale {

bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                           const int marginTop, const bool foregroundBlack, const bool needsTextGrayscale,
                           const bool needsImageGrayscale, uint8_t* scratch, const size_t scratchSize,
                           const bool asyncRefreshPending) {
  if ((!needsTextGrayscale && !needsImageGrayscale) || !renderer.supportsStripGrayscale()) {
    return false;
  }

  const int displayHeight = renderer.getDisplayHeight();
  const int displayWidthBytes = renderer.getDisplayWidthBytes();
  const size_t planeBytes = static_cast<size_t>(displayWidthBytes) * displayHeight;

  const auto renderPlaneToBuffer = [&](const GfxRenderer::RenderMode mode, uint8_t* buffer) {
    renderer.setRenderMode(mode);
    // A complete scratch plane needs only one page traversal. The target is
    // separate from the live BW framebuffer, just like an 80-row strip.
    renderer.beginStripTarget(buffer, 0, displayHeight);
    renderer.clearScreen(0x00);
    if (needsTextGrayscale) {
      page.render(renderer, fontId, marginLeft, marginTop, foregroundBlack);
    } else {
      page.renderImages(renderer, fontId, marginLeft, marginTop);
    }
    renderer.endStripTarget();
  };

  // Whole-plane buffers are about 48 KB each, so they are unsuitable for the
  // task stack or permanent activity storage. Each transient allocation must
  // leave enough total and contiguous heap for the next render allocations.
  constexpr size_t PLANE_BUFFER_FREE_HEAP_RESERVE = 60000;
  constexpr size_t PLANE_BUFFER_MAX_ALLOC_RESERVE = 16 * 1024;
  const bool usePsramPlanes = psramHeapAvailable();
  const auto planeBufferFits = [planeBytes, usePsramPlanes] {
    if (usePsramPlanes) {
      constexpr size_t PSRAM_PLANE_RESERVE = 128 * 1024;
      const auto psram = MemoryBudget::psramSnapshot();
      return psram.freeHeap >= planeBytes + PSRAM_PLANE_RESERVE && psram.maxAllocHeap >= planeBytes;
    }
    return ESP.getFreeHeap() >= planeBytes + PLANE_BUFFER_FREE_HEAP_RESERVE &&
           ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUFFER_MAX_ALLOC_RESERVE;
  };
  const auto allocatePlane = [planeBytes, usePsramPlanes] {
    return usePsramPlanes ? makePsramByteBufferNoThrow(planeBytes) : makeHeapByteBufferNoThrow(planeBytes);
  };
  auto lsbPlaneBuf = (asyncRefreshPending && planeBufferFits()) ? allocatePlane() : HeapByteBuffer{};
  auto msbPlaneBuf = (lsbPlaneBuf && planeBufferFits()) ? allocatePlane() : HeapByteBuffer{};

  if (lsbPlaneBuf) {
    if (usePsramPlanes) {
      LOG_INF("EPS", "Using PSRAM grayscale planes: bytes=%u count=%u", static_cast<unsigned>(planeBytes),
              msbPlaneBuf ? 2U : 1U);
    }
    renderPlaneToBuffer(GfxRenderer::GRAYSCALE_LSB, lsbPlaneBuf.get());
    if (msbPlaneBuf) {
      renderPlaneToBuffer(GfxRenderer::GRAYSCALE_MSB, msbPlaneBuf.get());
    }

    renderer.waitRefreshComplete();
    renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, displayHeight);
    if (msbPlaneBuf) {
      renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, displayHeight);
    } else {
      renderPlaneToBuffer(GfxRenderer::GRAYSCALE_MSB, lsbPlaneBuf.get());
      renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, displayHeight);
    }

    renderer.setRenderMode(GfxRenderer::BW);
    renderer.displayGrayBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    return true;
  }

  if (asyncRefreshPending) {
    // Controller writes and the BW snapshot fallback both need the refresh to
    // be complete when the whole-plane allocation cannot be satisfied.
    renderer.waitRefreshComplete();
  }

  const size_t requiredScratchSize = static_cast<size_t>(displayWidthBytes) * GRAYSCALE_STRIP_ROWS;
  if (!scratch || scratchSize < requiredScratchSize) {
    if (asyncRefreshPending) {
      // The shadow-free async update does not rebuild the controller's
      // differential baseline. Re-sync it even when grayscale is skipped.
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
    return false;
  }

  // Keep the live BW framebuffer intact, stream grayscale planes by row-band,
  // then re-sync the controller BW state from the framebuffer.
  const auto renderPlane = [&](const GfxRenderer::RenderMode mode, const bool lsbPlane) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < displayHeight; y += GRAYSCALE_STRIP_ROWS) {
      const int rows = std::min(GRAYSCALE_STRIP_ROWS, displayHeight - y);
      renderer.beginStripTarget(scratch, y, rows);
      renderer.clearScreen(0x00);
      if (needsTextGrayscale) {
        page.render(renderer, fontId, marginLeft, marginTop, foregroundBlack);
      } else {
        page.renderImages(renderer, fontId, marginLeft, marginTop);
      }
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch, y, rows);
    }
  };

  renderPlane(GfxRenderer::GRAYSCALE_LSB, true);

  renderPlane(GfxRenderer::GRAYSCALE_MSB, false);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}

}  // namespace EpubGrayscale
