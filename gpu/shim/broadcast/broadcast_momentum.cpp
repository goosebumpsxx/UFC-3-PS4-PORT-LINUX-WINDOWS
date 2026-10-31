// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 3. The momentum meter.
//
// A thin bar at the top of the screen that fills from the centre towards whoever is winning right
// now: +100 is the home fighter (left, red) fully on top, -100 the away fighter (right, blue). It is
// fed by the same stat store Phase 2 already reads — no new traps: every 0.1 s the fight totals of
// significant strikes landed, head strikes landed, takedowns, knockdowns and stuns are compared with
// the previous poll, and each rise credits the fighter who caused it. Between events the value drifts
// back towards 0, which is what makes it "right now" rather than "so far".
#include "broadcast_momentum.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "broadcast_config.h"
#include "broadcast_overlay.h"
#include "broadcast_stats.h"

namespace Broadcast {
namespace Momentum {
namespace {

using Stats::MomentumCounters;

constexpr std::uint32_t TrackColour = Rgba(10, 12, 18, 205);
constexpr std::uint32_t RimColour = Rgba(236, 241, 248, 95);
constexpr std::uint32_t HomeColour = Rgba(228, 48, 44, 255);
constexpr std::uint32_t AwayColour = Rgba(48, 116, 240, 255);
constexpr std::uint32_t CentreColour = Rgba(255, 255, 255, 150);

/// How often the store is asked. Fast enough for a live meter, far too slow to matter for a frame.
constexpr double PollSeconds = 0.1;
/// The drawn value chases the real one at this rate, so the bar glides instead of jumping.
constexpr float FollowRate = 9.0f;
/// One poll never credits more than this many events per counter: a re-allocated arena that resets a
/// counter cannot throw the bar across the screen.
constexpr int MaxEventsPerPoll = 40;
/// The bar: 34% of the width, near the top edge, above the game's own name and stamina bars.
constexpr float BarWidth = 0.34f;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct State {
    float value = 0.0f;  ///< what the fight has earned, decayed
    float shown = 0.0f;  ///< what is on screen
    int round = 0;
    bool haveBaseline = false;
    MomentumCounters previous[2];
    double lastPoll = 0.0;
    double lastFrame = 0.0;
    double lastInFight = 0.0;
};

State state;

int Rise(int after, int before) {
    const int delta = after - before;
    if (delta <= 0) {
        return 0;
    }
    return std::min(delta, MaxEventsPerPoll);
}

/// What one fighter's rises are worth, and a line of log for each non-zero poll (dump_stats only).
int PointsFor(const MomentumCounters& now, const MomentumCounters& before, const Config& config,
              const char* side, bool log) {
    const int sig = Rise(now.strikesLanded, before.strikesLanded);
    // A head strike is a significant strike too: it is worth the head value *instead of* the plain
    // one, so only the excess is counted twice when both counters rose.
    const int head = std::min(Rise(now.headStrikesLanded, before.headStrikesLanded), sig);
    const int takedowns = Rise(now.takedownsLanded, before.takedownsLanded);
    const int knockdowns = Rise(now.knockdowns, before.knockdowns);
    const int stuns = Rise(now.stuns, before.stuns);
    const int points = int(head * config.momentum_head_strike +
                           (sig - head) * config.momentum_sig_strike +
                           takedowns * config.momentum_takedown +
                           knockdowns * config.momentum_knockdown +
                           stuns * config.momentum_near_finish);
    if (log && points > 0) {
        std::printf("Broadcast: momentum: %s +%d (%d sig, %d head, %d takedowns, %d knockdowns, "
                    "%d stuns)\n",
                    side, points, sig - head, head, takedowns, knockdowns, stuns);
    }
    return points;
}

void Reset(int round) {
    state.value = 0.0f;
    state.shown = 0.0f;
    state.round = round;
    state.haveBaseline = false;
    state.lastInFight = Now();
}

void DrawMeter(const Canvas& canvas) {
    const float width = BarWidth * canvas.width;
    const float height = std::max(8.0f * canvas.scale, 5.0f);
    const float edge = std::max(1.0f * canvas.scale, 1.0f);
    const float left = 0.5f * (canvas.width - width);
    const float top = std::max(0.035f * canvas.height, 2.0f);
    const float radius = 0.5f * height;
    const float fraction = std::clamp(state.shown, -100.0f, 100.0f) / 100.0f;

    // A light rim around a dark track: the bar has to read against both a dark cage and a bright
    // canvas, and a flat dark strip that thin vanished into the picture.
    DrawRect(canvas, (left - edge) / canvas.width, (top - edge) / canvas.height,
             (width + 2.0f * edge) / canvas.width, (height + 2.0f * edge) / canvas.height,
             RimColour, radius + edge);
    DrawRect(canvas, left / canvas.width, top / canvas.height, width / canvas.width,
             height / canvas.height, TrackColour, radius);

    const float centre = left + 0.5f * width;
    const float filled = 0.5f * width * std::fabs(fraction);
    if (filled > 0.5f) {
        const float x = fraction > 0.0f ? centre : centre - filled;
        DrawRect(canvas, x / canvas.width, top / canvas.height, filled / canvas.width,
                 height / canvas.height, fraction > 0.0f ? HomeColour : AwayColour, radius);
    }
    // The centre mark: without it a bar sitting at 0 is indistinguishable from a bar that is off.
    DrawRect(canvas, (centre - 0.5f) / canvas.width, (top - 1.0f) / canvas.height,
             1.0f / canvas.width, (height + 2.0f) / canvas.height, CentreColour);
    if (GetConfig().dump_stats) {
        // A marker in the middle of the screen: it proves the meter's draw path is reached even when
        // the bar itself is at 0 and so thin that it hides in the picture.
        DrawRect(canvas, 0.5f, 0.5f, 0.02f, 0.02f, Rgba(255, 0, 255, 220));
    }
}

} // namespace

void Init() {}

void Tick(const Canvas& canvas) {
    const Config& config = GetConfig();
    if (!config.enabled || !config.momentum_meter) {
        // Off means off: no polling, no reads, no drawing.
        if (state.round != 0 || state.value != 0.0f) {
            Reset(0);
        }
        state.lastFrame = 0.0;
        return;
    }

    const double now = Now();
    const float dt = state.lastFrame > 0.0 ? float(std::min(now - state.lastFrame, 0.25)) : 0.0f;
    state.lastFrame = now;

    // Between fights there is nothing to watch. The clock stops for the round-start banner and for
    // replays, so a single unreadable frame is not the end of a fight: only a gap of a few seconds
    // clears the meter. Otherwise a banner mid-round would wipe momentum that the round earned.
    if (!Stats::InFight()) {
        if (state.round != 0 && now - state.lastInFight > 5.0) {
            Reset(0);
            std::printf("Broadcast: momentum: no fight, meter at 0\n");
        }
        state.lastPoll = 0.0;
        return;
    }
    state.lastInFight = now;

    const int round = Stats::Round();
    if (round != state.round) {
        Reset(round);
        std::printf("Broadcast: momentum: round %d, meter reset to 0\n", round);
    }

    // Drift: linear, so "about two points a second" is literally what the ini says.
    if (state.value > 0.0f) {
        state.value = std::max(0.0f, state.value - config.momentum_decay * dt);
    } else if (state.value < 0.0f) {
        state.value = std::min(0.0f, state.value + config.momentum_decay * dt);
    }

    // Events: one poll per tenth of a second, from the game's own fight totals.
    if (state.lastPoll == 0.0 || now - state.lastPoll >= PollSeconds) {
        state.lastPoll = now;
        MomentumCounters home;
        MomentumCounters away;
        if (Stats::ReadMomentum(1, home) && Stats::ReadMomentum(0, away)) {
            if (state.haveBaseline) {
                const int homePoints = PointsFor(home, state.previous[1], config, "home",
                                                 config.dump_stats);
                const int awayPoints = PointsFor(away, state.previous[0], config, "away",
                                                 config.dump_stats);
                if (homePoints != awayPoints) {
                    state.value = std::clamp(state.value + float(homePoints - awayPoints), -100.0f,
                                             100.0f);
                    if (config.dump_stats) {
                        std::printf("Broadcast: momentum: meter %.1f\n", double(state.value));
                    }
                }
            }
            state.previous[1] = home;
            state.previous[0] = away;
            state.haveBaseline = true;
        }
    }

    // Smoothing: the drawn value follows the real one instead of snapping to it.
    state.shown += (state.value - state.shown) * std::min(1.0f, dt * FollowRate);

    // Never over the pause menu or the between-rounds screen, like the Phase 2 card.
    if (Stats::Paused()) {
        return;
    }
    DrawMeter(canvas);
}

float Value() {
    return state.value;
}

} // namespace Momentum
} // namespace Broadcast
