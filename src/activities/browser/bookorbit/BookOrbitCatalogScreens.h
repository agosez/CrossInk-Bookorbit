#pragma once

#include <cstddef>

class GfxRenderer;
class MappedInputManager;

/**
 * The BookOrbit catalog's screens around its listing, drawn from plain values: the
 * activity decides which one shows and with what, these say how it looks. None of them
 * pushes the frame; the activity does once the screen is complete.
 */
namespace BookOrbitCatalogScreens {

// A server book list's header controls on touch devices, sized and spaced like the Library's.
constexpr int HEADER_CONTROL_SIZE = 44;
constexpr int HEADER_CONTROL_GAP = 10;

// The compact header and its title; on touch devices with a back button, the title then
// stopping short of the sort and filter controls when the listing has them (listControls).
void drawHeader(GfxRenderer& renderer, const MappedInputManager& input, const char* title, bool listControls);

// A single centred line while Wi-Fi comes up or a listing loads, with Back.
void drawStatus(GfxRenderer& renderer, const MappedInputManager& input, const char* message);

// The error heading and its message, wrapped, with Back.
void drawError(GfxRenderer& renderer, const MappedInputManager& input, const char* message);

// The book being downloaded, its progress once the size is known, and Cancel.
void drawDownload(GfxRenderer& renderer, const MappedInputManager& input, const char* title, size_t downloaded,
                  size_t total);

// The listing's button hints: Back, Confirm's action for the selected row, and on
// button-only devices Sort and Filter for a server book list (listButtons).
void drawListHints(GfxRenderer& renderer, const MappedInputManager& input, const char* confirmLabel, bool listButtons);

}  // namespace BookOrbitCatalogScreens
