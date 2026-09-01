#pragma once

#include <cstddef>

class GfxRenderer;
struct ThemeMetrics;

bool formatHeaderDateText(char* buffer, size_t length);
int headerDateReservedWidth(const GfxRenderer& renderer);
int headerDateLineBottomY(const GfxRenderer& renderer, const ThemeMetrics& metrics, int headerHeight = -1);
void drawHeaderDateAtBaseline(const GfxRenderer& renderer, int pageWidth, int baselineY);
void drawHeaderDateAtLineBottom(const GfxRenderer& renderer, int pageWidth, int lineBottomY);
void drawHeaderDate(const GfxRenderer& renderer, int pageWidth, const ThemeMetrics& metrics, int headerHeight = -1);
