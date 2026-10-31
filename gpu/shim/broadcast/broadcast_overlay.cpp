// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 1. Everything the broadcast features draw goes through
// GetForegroundDrawList(), so nothing takes focus or input, and every position is a viewport
// fraction times ui_scale — the same call lands in the same place at 720p, 1080p, 1440p and 4K.
#include "broadcast_overlay.h"

#include <algorithm>
#include <cstdio>

#include <SDL3/SDL_keycode.h>
#include "broadcast_config.h"
#include "imgui.h"

namespace Broadcast {

namespace {

bool hidden = false;

constexpr ImU32 PanelColour = IM_COL32(8, 10, 14, 170);
constexpr ImU32 TextColour = IM_COL32(238, 242, 246, 255);
constexpr ImU32 AccentColour = IM_COL32(255, 198, 64, 240);
constexpr ImU32 TrackColour = IM_COL32(255, 255, 255, 56);

/// Text is 22 px at 1080p, close to the port's own HUD text; it follows the display height the same
/// way BbOverlay::Render() scales ImGui (1080p = 1, never below 0.75).
constexpr float TextPixels = 22.0f;

enum class Align {
    Left,
    Centre,
    Right,
};

/// The frame the helpers draw into: display size in pixels plus the pixel scale (display height
/// times ui_scale).
struct Frame {
    float width = 1.0f;
    float height = 1.0f;
    float scale = 1.0f;
};

float PixelScale(float height, float ui_scale) {
    return std::max(height / 1080.0f, 0.75f) * ui_scale;
}

ImVec2 Measure(const Frame& frame, const char* text) {
    return ImGui::GetFont()->CalcTextSizeA(TextPixels * frame.scale, FLT_MAX, 0.0f, text);
}

/// Text at (x, y), both viewport fractions; y is the top of the line.
void Text(const Frame& frame, ImDrawList* draw, float x, float y, const char* text, ImU32 colour,
          Align align = Align::Left) {
    const ImVec2 measured = Measure(frame, text);
    float left = x * frame.width;
    if (align == Align::Centre) {
        left -= measured.x * 0.5f;
    } else if (align == Align::Right) {
        left -= measured.x;
    }
    draw->AddText(ImGui::GetFont(), TextPixels * frame.scale, ImVec2(left, y * frame.height), colour,
                  text);
}

/// Filled rectangle: (x, y) top-left and (w, h) size, all viewport fractions. `rounding` is pixels.
void Rect(const Frame& frame, ImDrawList* draw, float x, float y, float w, float h, ImU32 colour,
          float rounding = 0.0f) {
    draw->AddRectFilled(ImVec2(x * frame.width, y * frame.height),
                        ImVec2((x + w) * frame.width, (y + h) * frame.height), colour, rounding);
}

/// Horizontal bar: a track with a fill of `fraction` (clamped to 0..1), all viewport fractions.
void Bar(const Frame& frame, ImDrawList* draw, float x, float y, float w, float h, float fraction,
         ImU32 fill, ImU32 track) {
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const float rounding = 0.5f * h * frame.height;
    Rect(frame, draw, x, y, w, h, track, rounding);
    if (clamped > 0.0f) {
        Rect(frame, draw, x, y, w * clamped, h, fill, rounding);
    }
}

} // namespace

void Init() {
    LoadConfig();
    if (ToggleKeyCode() == std::uint32_t(SDLK_UNKNOWN)) {
        std::printf("Broadcast: toggle_key '%s' is not F1-F12 or A-Z, using F8\n",
                    GetConfig().toggle_key.c_str());
    }
}

bool Visible() {
    return GetConfig().enabled && !hidden;
}

void Draw(float width, float height) {
    if (!Visible() || width < 1.0f || height < 1.0f) {
        return;
    }
    const Config& config = GetConfig();
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.scale = PixelScale(height, config.ui_scale);
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // Phase 1 content: one line and a half-filled bar, bottom-centre, above the game's clock strip
    // (health and stamina sit top-left and top-right, the round timer at the very bottom).
    const char* label = "Broadcast mod loaded";
    const ImVec2 measured = Measure(frame, label);
    const float pad = 16.0f * frame.scale;
    const float gap = 10.0f * frame.scale;
    const float bar_height = 10.0f * frame.scale;
    const float bar_width = std::max(measured.x, 0.26f * width);
    const float box_width = bar_width + 2.0f * pad;
    const float box_height = 2.0f * pad + measured.y + gap + bar_height;
    const float box_left = 0.5f * (width - box_width);
    const float box_top = 0.82f * height - box_height;

    Rect(frame, draw, box_left / width, box_top / height, box_width / width, box_height / height,
         PanelColour, 8.0f * frame.scale);
    Text(frame, draw, 0.5f, (box_top + pad) / height, label, TextColour, Align::Centre);
    Bar(frame, draw, 0.5f - 0.5f * bar_width / width,
        (box_top + pad + measured.y + gap) / height, bar_width / width, bar_height / height, 0.5f,
        AccentColour, TrackColour);
}

bool HandleKey(std::uint32_t key, bool down, bool repeat) {
    if (!down || repeat || key != ToggleKeyCode()) {
        return false;
    }
    if (!GetConfig().enabled) {
        return false;
    }
    hidden = !hidden;
    std::printf("Broadcast: overlay %s\n", hidden ? "hidden" : "shown");
    return true;
}

} // namespace Broadcast
