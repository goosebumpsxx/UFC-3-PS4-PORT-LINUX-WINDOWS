// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 4. The judges' scorecard reveal.
//
// Source: FIGHTER_SCORE_CARD (id 7573, see broadcast_stats.cpp) — the game's own running cards. Its
// descriptor is count 12, strides {6, 1}, so one cell per fighter per round:
//
//     card[slot][0] = that fighter's running total      card[slot][1..5] = rounds 1..5
//
// and a round's value is the sum of all three judges: 30 when they all gave him the round, 27 when
// they all gave it away, 29/28 when they split 2-1. Slot 1 is home (red, left), slot 0 away (blue).
//
// Display only. Nothing here writes memory, and the game's decision is never touched.
#include "broadcast_scorecard.h"

#include <chrono>
#include <cstdio>

#include "broadcast_config.h"
#include "broadcast_overlay.h"
#include "broadcast_stats.h"

namespace Broadcast {
namespace Scorecard {
namespace {

constexpr std::uint32_t PanelColour = Rgba(6, 8, 12, 147); // 30 % more translucent than 210
constexpr std::uint32_t HomeColour = Rgba(255, 88, 78, 255);
constexpr std::uint32_t AwayColour = Rgba(86, 156, 255, 255);
constexpr std::uint32_t LabelColour = Rgba(176, 186, 198, 255);
constexpr std::uint32_t ValueColour = Rgba(240, 244, 248, 255);
constexpr std::uint32_t SplitColour = Rgba(240, 196, 90, 255);

constexpr float FadeSeconds = 0.5f;
/// Clear of the game's own ticked momentum bar (2.8 % of the height, Phase 3 §1) and of the
/// health/stamina HUD at the top-left.
constexpr float PanelTopFraction = 0.115f;
constexpr float PanelWidthFraction = 0.34f;

struct State {
    bool shown = false;
    double shownAt = 0.0;
    float duration = 6.0f;
    int round = 0;      ///< the round whose cards are up: the one that just finished
    int lastRound = 0;  ///< the round the ticker saw last, to catch the bell
    Stats::Scorecard card;
};

State state;

double Now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point started = clock::now();
    return std::chrono::duration<double>(clock::now() - started).count();
}

std::uint32_t Fade(std::uint32_t colour, float alpha) {
    const float clamped = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const std::uint32_t a = std::uint32_t(255.0f * clamped) & 0xffu;
    return (colour & 0x00ffffffu) | (a << 24);
}

/// 30 for a round all three judges gave him, 27 for one they all gave away, 29/28 when they split.
bool WasSplit(int home, int away) {
    const int high = home > away ? home : away;
    const int low = home > away ? away : home;
    return !(high == 30 && low == 27);
}

/// Laid out in pixels and converted to fractions at the call, like Phase 2's card: the canvas
/// helpers take fractions for positions and pixels for sizes, and mixing them puts the panel off
/// screen.
void DrawPanel(const Canvas& canvas) {
    const double elapsed = Now() - state.shownAt;
    const float remaining = state.duration - float(elapsed);
    const float alpha = remaining > FadeSeconds ? 1.0f : remaining / FadeSeconds;

    const int rounds = state.card.rounds < 1 ? 1 : (state.card.rounds > 5 ? 5 : state.card.rounds);
    const float font = TextLineHeight(canvas);
    const float line = font * 1.35f;
    const float pad = 14.0f * canvas.scale;
    const float width = PanelWidthFraction * canvas.width;
    const float height = pad * 2.0f + line * float(rounds + 3);
    const float left = 0.5f * (canvas.width - width);
    const float top = PanelTopFraction * canvas.height;

    DrawRect(canvas, left / canvas.width, top / canvas.height, width / canvas.width,
             height / canvas.height, Fade(PanelColour, alpha), 8.0f * canvas.scale);

    const float centre = 0.5f;
    const float textLeft = (left + pad) / canvas.width;
    const float textRight = (left + width - pad) / canvas.width;
    float y = top + pad;

    DrawTextAt(canvas, centre, y / canvas.height, "ROUND SCORES", Fade(LabelColour, alpha),
               TextAlign::Centre, font);
    y += line;

    int homeRounds = 0;
    int awayRounds = 0;
    for (int round = 1; round <= rounds; ++round) {
        const int home = state.card.score[1][round - 1];
        const int away = state.card.score[0][round - 1];
        if (home == 0 && away == 0) {
            continue;
        }
        if (WasSplit(home, away)) {
            // 29/28: the three judges did not all agree on this round.
        }
        if (home > away) {
            ++homeRounds;
        } else if (away > home) {
            ++awayRounds;
        }

        char label[16];
        std::snprintf(label, sizeof(label), "R%d", round);
        DrawTextAt(canvas, textLeft, y / canvas.height, label, Fade(LabelColour, alpha),
                   TextAlign::Left, font);

        char homeText[16];
        char awayText[16];
        std::snprintf(homeText, sizeof(homeText), "%d", home);
        std::snprintf(awayText, sizeof(awayText), "%d", away);
        DrawTextAt(canvas, centre - 0.014f, y / canvas.height, homeText, Fade(HomeColour, alpha),
                   TextAlign::Right, font);
        DrawTextAt(canvas, centre + 0.014f, y / canvas.height, awayText, Fade(AwayColour, alpha),
                   TextAlign::Left, font);
        if (WasSplit(home, away)) {
            DrawTextAt(canvas, textRight, y / canvas.height, "2-1", Fade(SplitColour, alpha),
                       TextAlign::Right, font);
        }
        y += line;
    }

    // The running total, then who leads on the rounds actually scored.
    DrawTextAt(canvas, textLeft, y / canvas.height, "TOTAL", Fade(LabelColour, alpha),
               TextAlign::Left, font);
    char totalHome[16];
    char totalAway[16];
    std::snprintf(totalHome, sizeof(totalHome), "%d", state.card.total[1]);
    std::snprintf(totalAway, sizeof(totalAway), "%d", state.card.total[0]);
    DrawTextAt(canvas, centre - 0.014f, y / canvas.height, totalHome, Fade(ValueColour, alpha),
               TextAlign::Right, font);
    DrawTextAt(canvas, centre + 0.014f, y / canvas.height, totalAway, Fade(ValueColour, alpha),
               TextAlign::Left, font);
    y += line;

    char summary[64];
    const int scored = homeRounds + awayRounds;
    if (homeRounds == awayRounds) {
        std::snprintf(summary, sizeof(summary), "EVEN ON ROUNDS (%d-%d)", homeRounds, awayRounds);
    } else if (homeRounds > awayRounds) {
        std::snprintf(summary, sizeof(summary), "RED LEADS %d OF %d ROUNDS", homeRounds, scored);
    } else {
        std::snprintf(summary, sizeof(summary), "BLUE LEADS %d OF %d ROUNDS", awayRounds, scored);
    }
    DrawTextAt(canvas, centre, y / canvas.height, summary, Fade(ValueColour, alpha),
               TextAlign::Centre, font);
}

} // namespace

void Init() {}

void Tick(const Canvas& canvas) {
    const Config& config = GetConfig();
    if (!config.enabled || !config.scorecard_reveal) {
        state.shown = false;
        return;
    }
    if (!Stats::InFight()) {
        state.shown = false;
        state.lastRound = 0;
        return;
    }

    const double now = Now();
    const int round = Stats::Round();
    if (round > state.lastRound) {
        // The bell: the round that just finished now has its cards, so reveal them once.
        if (state.lastRound != 0) {
            Stats::Scorecard card;
            if (Stats::ReadScorecard(card) && card.rounds > 0) {
                state.card = card;
                state.round = state.lastRound;
                state.shown = true;
                state.shownAt = now;
                state.duration = config.scorecard_seconds;
                std::printf("Broadcast: scorecard: %d round(s) on the cards, total %d-%d\n",
                            card.rounds, card.total[1], card.total[0]);
            }
        }
        state.lastRound = round;
    }

    if (!state.shown) {
        return;
    }
    if (now - state.shownAt > double(state.duration)) {
        state.shown = false;
        return;
    }
    // Never over the pause menu or the between-rounds screen.
    if (Stats::Paused()) {
        return;
    }
    DrawPanel(canvas);
}

} // namespace Scorecard
} // namespace Broadcast
