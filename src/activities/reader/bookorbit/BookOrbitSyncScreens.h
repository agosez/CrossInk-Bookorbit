#pragma once

#include <cstddef>
#include <string>

#include "components/TouchActionButtons.h"

class GfxRenderer;
struct ThemeMetrics;

/**
 * The BookOrbit sync's screens below the title band, drawn from plain values: the activity
 * decides which one shows and with what, these say how it looks. Each layout here serves
 * both the draw and the hit test, so a translated label cannot make the two drift apart.
 *
 * The result screen mirrors KOReaderSyncActivity's: the progress cards and their "Ahead"
 * badge arrived there with CrossInk v1.6.1. The two screens ask the same question with the
 * same two answers; a shared helper would have to live in KOReaderSyncActivity.cpp, which
 * is upstream's, and would conflict on every sync.
 */
namespace BookOrbitSyncScreens {

struct ProgressCard {
  const char* title;
  const char* source;  // Optional, e.g. the remote device name.
  const char* pageText;
  const char* chapter;
  float percentage;  // 0.0 - 1.0
};

// Apply/Upload buttons are pinned above the button hints (or the bottom edge on touch devices) so the
// progress cards can use the space above them.
TouchActionButtons::Layout resultActionLayout(const Rect& screen, const ThemeMetrics& metrics, bool hasTouch);

TouchActionButtons::Layout noRemoteProgressActionLayout(const Rect& screen, const ThemeMetrics& metrics);

// The phase under way: its status message, then either a summary's lines or the phase's
// "done / total" count and bar (0 total = no bar).
void drawStatus(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, int top,
                const char* status, const std::string (&summaryLines)[2], size_t progressDone, size_t progressTotal);

// The choice between the two positions: a card for each, the one further along marked
// "Ahead", and the Apply/Upload buttons with selectedOption highlighted.
void drawResult(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, int top,
                const ProgressCard& remote, const ProgressCard& local, int selectedOption, bool hasTouch);

void drawFailure(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, int top,
                 const char* message);

}  // namespace BookOrbitSyncScreens
