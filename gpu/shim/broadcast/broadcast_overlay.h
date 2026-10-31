// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 1. The overlay itself: text, rectangles, bars. See
// broadcast_overlay.cpp.
#pragma once

#include <cstdint>

namespace Broadcast {

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
