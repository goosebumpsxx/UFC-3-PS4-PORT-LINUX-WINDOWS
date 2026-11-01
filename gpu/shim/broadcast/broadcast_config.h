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
    float scorecard_seconds = 6.0f; // how long the Phase 4 card reveal stays up
    float ui_scale = 1.0f;
    std::string toggle_key = "F8";
    // Phase 2: the stat card.
    float popup_seconds = 5.0f;     // how long the previous-round card stays up
    float popup_delay = 15.0f;      // seconds into rounds 2+ before it appears
    float knockdown_seconds = 3.0f; // the shorter knockdown card
    std::string card_key = "F7";   // on demand: the fight so far
    bool debug_card = false;        // the Phase 1 "Broadcast mod loaded" demo card
    bool dump_stats = false;        // log this fight's raw store cells on the card key
    // Phase 3: the momentum meter. Points per event and how fast it drifts back to the centre. The
    // values are tuned so that a three or four strike flurry visibly moves our bar, which the game's
    // own ticked bar does not show.
    float momentum_sig_strike = 8.0f;   // significant strike landed
    float momentum_head_strike = 10.0f;  // significant head strike landed (instead of the above)
    float momentum_takedown = 18.0f;    // takedown landed
    float momentum_knockdown = 40.0f;   // knockdown scored
    float momentum_near_finish = 16.0f; // stun/wobble forced (the game's own Stuns/Wobbles row)
    float momentum_decay = 1.0f;        // points per second back towards 0
    float momentum_even_margin = 3.0f;  // a round within this many points is EVEN
    // Fighter names for the meter's labels. Empty means the corner: "RED" (home) or "BLUE" (away),
    // which is all the Phase 2 card shows — the store carries no names.
    std::string momentum_home_name;
    std::string momentum_away_name;
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
