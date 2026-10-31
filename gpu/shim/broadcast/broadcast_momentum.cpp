// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 3. The momentum meter, and the numbers the game's own meter
// does not show.
//
// The game has a short ticked momentum bar of its own, right under this one. Ours is the long thin bar
// plus the text around it: a bar that moves further than the game's, the points each fighter has earned
// this round, who leads the round, what just scored and who has taken the rounds so far. It is fed by
// the same stat store Phase 2 already reads — no new traps: every 0.1 s the fight totals of significant
// strikes landed, head strikes landed, takedowns, knockdowns and stuns are compared with the previous
// poll, and each rise credits the fighter who caused it.
#include "broadcast_momentum.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
constexpr std::uint32_t CentreColour = Rgba(255, 255, 255, 170);
constexpr std::uint32_t LabelHome = Rgba(255, 88, 78, 255);
constexpr std::uint32_t LabelAway = Rgba(86, 156, 255, 255);
constexpr std::uint32_t LeaderColour = Rgba(238, 242, 246, 240);
constexpr std::uint32_t DimColour = Rgba(238, 242, 246, 170);

/// How often the store is asked. Fast enough for a live meter, far too slow to matter for a frame.
constexpr double PollSeconds = 0.1;
/// The drawn value chases the real one at this rate, so the bar glides instead of jumping.
constexpr float FollowRate = 9.0f;
/// One poll never credits more than this many events per counter: a re-allocated arena that resets a
/// counter cannot throw the bar across the screen.
constexpr int MaxEventsPerPoll = 40;
/// The bar: 34% of the width. It sits above the game's own ticked momentum bar, never on it.
constexpr float BarWidth = 0.34f;
/// How long the last-event tag stays up.
constexpr double EventTagSeconds = 2.0;
/// Text sizes, in pixels at 1080p.
constexpr float ScorePixels = 20.0f;
constexpr float LeaderPixels = 16.0f;
constexpr float TagPixels = 14.0f;
constexpr float SlatePixels = 14.0f;
/// Names longer than this would run into the bar; the meter is a caption, not a header.
constexpr std::size_t MaxNameChars = 12;

enum class Kind {
    None,
    Strike,
    HeadStrike,
    Takedown,
    Knockdown,
    Stun,
};

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Strike:
        return "STRIKE";
    case Kind::HeadStrike:
        return "HEAD STRIKE";
    case Kind::Takedown:
        return "TAKEDOWN";
    case Kind::Knockdown:
        return "KNOCKDOWN";
    case Kind::Stun:
        return "STUN";
    case Kind::None:
        break;
    }
    return "";
}

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
    /// Points each fighter has earned in the round being fought, with no decay: [0] away, [1] home.
    int roundPoints[2] = {0, 0};
    /// Round winners by round number: 0 nobody (even or unfinished), 1 home, 2 away.
    int roundWinner[6] = {0, 0, 0, 0, 0, 0};
    /// The last scoring poll, for the tag: who, what, how much, and when.
    Kind eventKind = Kind::None;
    int eventPoints = 0;
    int eventSide = 1;
    double eventAt = 0.0;
};

State state;

int Rise(int after, int before) {
    const int delta = after - before;
    if (delta <= 0) {
        return 0;
    }
    return std::min(delta, MaxEventsPerPoll);
}

/// What one fighter's rises in one poll are worth, and the most significant single thing among them.
struct Credit {
    int points = 0;
    Kind heaviest = Kind::None;
    int heaviestPoints = 0;
};

Credit Score(const MomentumCounters& now, const MomentumCounters& before, const Config& config,
             const char* side, bool log) {
    const int sig = Rise(now.strikesLanded, before.strikesLanded);
    // A head strike is a significant strike too: it is worth the head value *instead of* the plain
    // one, so only the excess is counted twice when both counters rose.
    const int head = std::min(Rise(now.headStrikesLanded, before.headStrikesLanded), sig);
    const int takedowns = Rise(now.takedownsLanded, before.takedownsLanded);
    const int knockdowns = Rise(now.knockdowns, before.knockdowns);
    const int stuns = Rise(now.stuns, before.stuns);

    Credit credit;
    credit.points = int(head * config.momentum_head_strike +
                        (sig - head) * config.momentum_sig_strike +
                        takedowns * config.momentum_takedown +
                        knockdowns * config.momentum_knockdown +
                        stuns * config.momentum_near_finish);
    if (log && credit.points > 0) {
        std::printf("Broadcast: momentum: %s +%d (%d sig, %d head, %d takedowns, %d knockdowns, "
                    "%d stuns)\n",
                    side, credit.points, sig - head, head, takedowns, knockdowns, stuns);
    }

    // The tag names the single most important thing that scored, in the order a broadcast would say
    // them, so a poll that scored two strikes and a knockdown says "KNOCKDOWN".
    struct Candidate {
        Kind kind;
        int count;
        float worth;
    };
    const Candidate candidates[] = {
        {Kind::Knockdown, knockdowns, config.momentum_knockdown},
        {Kind::Stun, stuns, config.momentum_near_finish},
        {Kind::Takedown, takedowns, config.momentum_takedown},
        {Kind::HeadStrike, head, config.momentum_head_strike},
        {Kind::Strike, sig - head, config.momentum_sig_strike},
    };
    for (const Candidate& candidate : candidates) {
        if (candidate.count > 0 && candidate.worth > 0.0f) {
            credit.heaviest = candidate.kind;
            credit.heaviestPoints = int(candidate.worth);
            break;
        }
    }
    return credit;
}

/// The name to print for a side: the ini's name, or the corner when it has none.
const char* SideName(int slot) {
    const Config& config = GetConfig();
    const std::string& name = slot == 1 ? config.momentum_home_name : config.momentum_away_name;
    if (!name.empty()) {
        return name.c_str();
    }
    return slot == 1 ? "RED" : "BLUE";
}

/// The side's name cut to MaxNameChars, for a caller that hands it to printf.
void ShortName(int slot, char* out, std::size_t size) {
    const char* name = SideName(slot);
    const std::size_t length = std::min(std::strlen(name), MaxNameChars);
    std::snprintf(out, size, "%.*s", int(length), name);
}

/// Close the round that is ending: whoever led it by more than the even margin takes it.
void CloseRound(int round) {
    if (round < 1 || round > 5) {
        return;
    }
    const int diff = state.roundPoints[1] - state.roundPoints[0];
    const int margin = int(GetConfig().momentum_even_margin);
    state.roundWinner[round] = diff > margin ? 1 : (diff < -margin ? 2 : 0);
}

void Reset(int round) {
    // A round just finished, or the fight did. A round number that does not go up is a new fight, so
    // the rounds-won slate starts over with it.
    const bool newFight = round == 0 || round <= state.round;
    if (round != state.round) {
        CloseRound(state.round);
    }
    if (newFight) {
        for (int& winner : state.roundWinner) {
            winner = 0;
        }
    }
    state.value = 0.0f;
    state.shown = 0.0f;
    state.round = round;
    state.haveBaseline = false;
    state.roundPoints[0] = 0;
    state.roundPoints[1] = 0;
    state.eventKind = Kind::None;
    state.lastInFight = Now();
}

void DrawBar(const Canvas& canvas, float top, float height, float left, float width) {
    const float edge = std::max(1.0f * canvas.scale, 1.0f);
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
        // Home is the left corner and the red fill, away the right corner and the blue one: the fill
        // grows towards the corner of whoever is winning.
        const bool home = fraction > 0.0f;
        const float x = home ? centre - filled : centre;
        DrawRect(canvas, x / canvas.width, top / canvas.height, filled / canvas.width,
                 height / canvas.height, home ? HomeColour : AwayColour, radius);
    }
    // The centre mark: without it a bar sitting at 0 is indistinguishable from a bar that is off.
    DrawRect(canvas, (centre - 0.5f) / canvas.width, (top - 1.0f) / canvas.height,
             1.0f / canvas.width, (height + 2.0f) / canvas.height, CentreColour);
}

/// The slate of rounds decided so far: "R1 RED  R2 BLUE", or nothing before a round has finished.
void DrawSlate(const Canvas& canvas, float y, const char* home, const char* away) {
    if (state.round < 2) {
        return;
    }
    char text[192];
    std::size_t used = 0;
    text[0] = '\0';
    for (int round = 1; round < state.round && round <= 5; ++round) {
        const int winner = state.roundWinner[round];
        const char* who = winner == 1 ? home : (winner == 2 ? away : "EVEN");
        const int written = std::snprintf(text + used, sizeof(text) - used, "%sR%d %s",
                                          used == 0 ? "" : "   ", round, who);
        if (written <= 0 || std::size_t(written) >= sizeof(text) - used) {
            break;
        }
        used += std::size_t(written);
    }
    if (used == 0) {
        return;
    }
    DrawTextAt(canvas, 0.5f, y, text, DimColour, TextAlign::Centre, SlatePixels);
}

void DrawMeter(const Canvas& canvas) {
    const Config& config = GetConfig();
    const float width = BarWidth * canvas.width;
    const float height = std::max(10.0f * canvas.scale, 6.0f);
    const float left = 0.5f * (canvas.width - width);
    const float right = left + width;
    // Above the game's own ticked momentum bar, which sits about 4.6% of the height down.
    const float top = std::max(0.028f * canvas.height, 2.0f);
    // The scores sit on the bar's line, in the free space outside its two ends.
    const float scoreY = top + 0.5f * height - 0.5f * TextLineHeightAt(canvas, ScorePixels);
    const float underY = top + 0.5f * height + 0.5f * TextLineHeightAt(canvas, ScorePixels) + 2.0f;
    const float leaderY = std::max(top - TextLineHeightAt(canvas, LeaderPixels) - 6.0f, 2.0f);
    const float gap = 14.0f * canvas.scale;

    DrawBar(canvas, top, height, left, width);

    if (state.round < 1) {
        return;
    }

    char home[MaxNameChars + 1];
    char away[MaxNameChars + 1];
    ShortName(1, home, sizeof home);
    ShortName(0, away, sizeof away);

    // 1. The points each fighter has earned this round, at each end of the bar, in corner colour.
    char text[96];
    std::snprintf(text, sizeof text, "%s %d", home, state.roundPoints[1]);
    DrawTextAt(canvas, (left - gap) / canvas.width, scoreY / canvas.height, text, LabelHome,
               TextAlign::Right, ScorePixels);
    std::snprintf(text, sizeof text, "%d %s", state.roundPoints[0], away);
    DrawTextAt(canvas, (right + gap) / canvas.width, scoreY / canvas.height, text, LabelAway,
               TextAlign::Left, ScorePixels);

    // 2. Who leads the round, above the bar.
    const int edgePoints = state.roundPoints[1] - state.roundPoints[0];
    if (std::abs(edgePoints) <= int(config.momentum_even_margin)) {
        std::snprintf(text, sizeof text, "ROUND %d: EVEN", state.round);
    } else {
        std::snprintf(text, sizeof text, "ROUND %d EDGE: %s +%d", state.round,
                      edgePoints > 0 ? home : away, std::abs(edgePoints));
    }
    DrawTextAt(canvas, 0.5f, leaderY / canvas.height, text, LeaderColour, TextAlign::Centre,
               LeaderPixels);

    // 3. What just scored, for a couple of seconds, in the colour of the fighter who scored it.
    if (state.eventKind != Kind::None && Now() - state.eventAt <= EventTagSeconds) {
        std::snprintf(text, sizeof text, "+%d %s %s", state.eventPoints,
                      KindName(state.eventKind), state.eventSide == 1 ? home : away);
        const bool homeSide = state.eventSide == 1;
        DrawTextAt(canvas, (homeSide ? left - gap : right + gap) / canvas.width,
                   underY / canvas.height, text, homeSide ? LabelHome : LabelAway,
                   homeSide ? TextAlign::Right : TextAlign::Left, TagPixels);
    }

    // 4. The rounds decided so far, under the bar, clear of the game's own momentum bar.
    DrawSlate(canvas, (underY + TextLineHeightAt(canvas, TagPixels) + 2.0f) / canvas.height, home,
              away);

    if (config.dump_stats) {
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
                const Credit homeCredit =
                    Score(home, state.previous[1], config, "home", config.dump_stats);
                const Credit awayCredit =
                    Score(away, state.previous[0], config, "away", config.dump_stats);
                if (homeCredit.points != awayCredit.points) {
                    state.value = std::clamp(
                        state.value + float(homeCredit.points - awayCredit.points), -100.0f, 100.0f);
                    if (config.dump_stats) {
                        std::printf("Broadcast: momentum: meter %.1f, round score %d-%d\n",
                                    double(state.value), state.roundPoints[1],
                                    state.roundPoints[0]);
                    }
                }
                state.roundPoints[1] += homeCredit.points;
                state.roundPoints[0] += awayCredit.points;
                // The tag shows the bigger single thing that scored in this poll.
                if (homeCredit.heaviest != Kind::None || awayCredit.heaviest != Kind::None) {
                    const bool homeWins =
                        awayCredit.heaviest == Kind::None ||
                        (homeCredit.heaviest != Kind::None &&
                         homeCredit.heaviestPoints >= awayCredit.heaviestPoints);
                    state.eventKind = homeWins ? homeCredit.heaviest : awayCredit.heaviest;
                    state.eventPoints =
                        homeWins ? homeCredit.heaviestPoints : awayCredit.heaviestPoints;
                    state.eventSide = homeWins ? 1 : 0;
                    state.eventAt = now;
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
