// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 1. The overlay itself: text, rectangles, bars. See
// broadcast_overlay.cpp.
#pragma once

#include <cstdint>

namespace Broadcast {

/// The drawing frame: display size in pixels plus the pixel scale (display height over 1080, never
/// below 0.75, times ui_scale). Every position in this module is a fraction of the frame.
struct Canvas {
    float width = 1.0f;
    float height = 1.0f;
    float scale = 1.0f;
};

enum class TextAlign {
    Left,
    Centre,
    Right,
};

/// The canvas for one frame at `ui_scale`. Both Phase 1's demo card and Phase 2's stat card draw
/// through the helpers below, so they land in the same place at every resolution.
Canvas MakeCanvas(float width, float height);

/// Packed RGBA in the order ImGui wants it (0xAABBGGRR).
constexpr std::uint32_t Rgba(int r, int g, int b, int a) {
    return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16) |
           (std::uint32_t(a) << 24);
}

float TextWidth(const Canvas& canvas, const char* text);
float TextLineHeight(const Canvas& canvas);

/// The same, at an explicit size in pixels (times the canvas scale): the momentum meter's tags and
/// slates are smaller than the cards' rows.
float TextWidthAt(const Canvas& canvas, const char* text, float pixels);
float TextLineHeightAt(const Canvas& canvas, float pixels);

/// Text at (x, y) in fractions and at `pixels`; y is the top of the line.
void DrawTextAt(const Canvas& canvas, float x, float y, const char* text, std::uint32_t colour,
                TextAlign align, float pixels);

/// Text at (x, y) in fractions; y is the top of the line.
void DrawText(const Canvas& canvas, float x, float y, const char* text, std::uint32_t colour,
              TextAlign align = TextAlign::Left);

/// Filled rectangle: (x, y) top-left and (w, h) size, all fractions. `rounding` is in pixels.
void DrawRect(const Canvas& canvas, float x, float y, float w, float h, std::uint32_t colour,
              float rounding = 0.0f);

/// Horizontal bar: a track with a fill of `fraction` (clamped to 0..1).
void DrawBar(const Canvas& canvas, float x, float y, float w, float h, float fraction,
             std::uint32_t fill, std::uint32_t track);

/// Reads broadcast_mod.ini. Called once from BbOverlay::Init(), before the first frame.
void Init();

/// True while the mod draws: enabled in the ini and not hidden with the toggle key.
bool Visible();

/// Draws the overlay after ImGui::NewFrame(), in display pixels. Never called while the settings
/// menu is open, so the menu stays readable.
void Draw(float width, float height);

/// Key events from BbOverlay::HandleEvent(). Returns true when the key was ours; the caller never
/// consumes it, so the game keeps seeing every key it maps.
bool HandleKey(std::uint32_t key, bool down, bool repeat);

} // namespace Broadcast
