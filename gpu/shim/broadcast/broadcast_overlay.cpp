// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 1. Everything the broadcast features draw goes through
// GetForegroundDrawList(), so nothing takes focus or input, and every position is a viewport
// fraction times ui_scale — the same call lands in the same place at 720p, 1080p, 1440p and 4K.
#include "broadcast_overlay.h"

#include <algorithm>
#include <cstdio>

#include <SDL3/SDL_keycode.h>
#include "broadcast_card.h"
#include "broadcast_config.h"
#include "broadcast_stats.h"
#include "imgui.h"

namespace Broadcast {

namespace {

bool hidden = false;

constexpr std::uint32_t PanelColour = Rgba(8, 10, 14, 170);
constexpr std::uint32_t TextColour = Rgba(238, 242, 246, 255);
constexpr std::uint32_t AccentColour = Rgba(255, 198, 64, 240);
constexpr std::uint32_t TrackColour = Rgba(255, 255, 255, 56);

/// Text is 22 px at 1080p, close to the port's own HUD text; it follows the display height the same
/// way BbOverlay::Render() scales ImGui (1080p = 1, never below 0.75).
constexpr float TextPixels = 22.0f;

float PixelScale(float height, float ui_scale) {
    return std::max(height / 1080.0f, 0.75f) * ui_scale;
}

ImVec2 Measure(const Canvas& canvas, const char* text) {
    return ImGui::GetFont()->CalcTextSizeA(TextPixels * canvas.scale, FLT_MAX, 0.0f, text);
}

/// Phase 1's demo card: one line and a half-filled bar, bottom-centre, above the game's clock strip
/// (health and stamina sit top-left and top-right, the round timer at the very bottom). Phase 2 keeps
/// it behind `debug_card` now that a real card exists.
void DrawDemoCard(const Canvas& canvas) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const char* label = "Broadcast mod loaded";
    const ImVec2 measured = Measure(canvas, label);
    const float pad = 16.0f * canvas.scale;
    const float gap = 10.0f * canvas.scale;
    const float bar_height = 10.0f * canvas.scale;
    const float bar_width = std::max(measured.x, 0.26f * canvas.width);
    const float box_width = bar_width + 2.0f * pad;
    const float box_height = 2.0f * pad + measured.y + gap + bar_height;
    const float box_left = 0.5f * (canvas.width - box_width);
    const float box_top = 0.82f * canvas.height - box_height;

    DrawRect(canvas, box_left / canvas.width, box_top / canvas.height, box_width / canvas.width,
             box_height / canvas.height, PanelColour, 8.0f * canvas.scale);
    DrawText(canvas, 0.5f, (box_top + pad) / canvas.height, label, TextColour, TextAlign::Centre);
    DrawBar(canvas, 0.5f - 0.5f * bar_width / canvas.width,
            (box_top + pad + measured.y + gap) / canvas.height, bar_width / canvas.width,
            bar_height / canvas.height, 0.5f, AccentColour, TrackColour);
    (void)draw;
}

} // namespace

Canvas MakeCanvas(float width, float height) {
    Canvas canvas;
    canvas.width = std::max(width, 1.0f);
    canvas.height = std::max(height, 1.0f);
    canvas.scale = PixelScale(height, GetConfig().ui_scale);
    return canvas;
}

float TextWidth(const Canvas& canvas, const char* text) {
    return Measure(canvas, text).x;
}

float TextLineHeight(const Canvas& canvas) {
    return Measure(canvas, "0").y;
}

void DrawText(const Canvas& canvas, float x, float y, const char* text, std::uint32_t colour,
              TextAlign align) {
    const ImVec2 measured = Measure(canvas, text);
    float left = x * canvas.width;
    if (align == TextAlign::Centre) {
        left -= measured.x * 0.5f;
    } else if (align == TextAlign::Right) {
        left -= measured.x;
    }
    ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), TextPixels * canvas.scale,
                                            ImVec2(left, y * canvas.height), colour, text);
}

void DrawRect(const Canvas& canvas, float x, float y, float w, float h, std::uint32_t colour,
              float rounding) {
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(x * canvas.width, y * canvas.height),
                                                  ImVec2((x + w) * canvas.width,
                                                         (y + h) * canvas.height),
                                                  colour, rounding);
}

void DrawBar(const Canvas& canvas, float x, float y, float w, float h, float fraction,
             std::uint32_t fill, std::uint32_t track) {
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const float rounding = 0.5f * h * canvas.height;
    DrawRect(canvas, x, y, w, h, track, rounding);
    if (clamped > 0.0f) {
        DrawRect(canvas, x, y, w * clamped, h, fill, rounding);
    }
}

void Init() {
    LoadConfig();
    if (ToggleKeyCode() == std::uint32_t(SDLK_UNKNOWN)) {
        std::printf("Broadcast: toggle_key '%s' is not F1-F12 or A-Z, using F8\n",
                    GetConfig().toggle_key.c_str());
    }
    Stats::Init();
    Card::Init();
}

bool Visible() {
    return GetConfig().enabled && !hidden;
}

void Draw(float width, float height) {
    if (!Visible() || width < 1.0f || height < 1.0f) {
        return;
    }
    const Config& config = GetConfig();
    if (!config.enabled) {
        return;
    }
    const Canvas canvas = MakeCanvas(width, height);
    Stats::Tick();

    // `stat_popups = false` keeps the traps and the reads away entirely: the card module never runs.
    if (config.stat_popups) {
        Card::Tick(canvas);
    }
    if (config.debug_card) {
        DrawDemoCard(canvas);
    }
}

bool HandleKey(std::uint32_t key, bool down, bool repeat) {
    if (!down) {
        return false;
    }
    if (!GetConfig().enabled) {
        return false;
    }
    if (!repeat && Card::HandleKey(key)) {
        return true;
    }
    if (repeat || key != ToggleKeyCode()) {
        return false;
    }
    hidden = !hidden;
    std::printf("Broadcast: overlay %s\n", hidden ? "hidden" : "shown");
    return true;
}

} // namespace Broadcast
