// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 1. Settings file (broadcast_mod.ini). See broadcast_config.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace Broadcast {

/// Everything broadcast_mod.ini can set. Missing file = these values.
struct Config {
    bool enabled = true;
    bool stat_popups = true;
    bool momentum_meter = true;
    bool scorecard_reveal = true;
    float ui_scale = 1.0f;
    std::string toggle_key = "F8";
    // Phase 2: the stat card.
    float popup_seconds = 5.0f;     // how long the previous-round card stays up
    float popup_delay = 15.0f;      // seconds into rounds 2+ before it appears
    float knockdown_seconds = 3.0f; // the shorter knockdown card
    std::string card_key = "F7";   // on demand: the fight so far
    bool debug_card = false;        // the Phase 1 "Broadcast mod loaded" demo card
    bool dump_stats = false;        // log this fight's raw store cells on the card key
};

/// Reads broadcast_mod.ini from the folder of bbport.ini (BB_CONFIG), writing the defaults when the
/// file is missing. Called once, from Broadcast::Init().
void LoadConfig();

/// The settings, valid after LoadConfig(). Read per frame, so no copies.
const Config& GetConfig();

/// The toggle key as an SDL keycode, or SDLK_UNKNOWN when toggle_key names a key we do not know.
std::uint32_t ToggleKeyCode();

/// The on-demand card key as an SDL keycode (SDLK_UNKNOWN when card_key is not a key we know).
std::uint32_t CardKeyCode();

} // namespace Broadcast
