// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 2. Live fight statistics, read out of guest memory.
// See broadcast_stats.cpp for the design.
#pragma once

#include <cstdint>

namespace Broadcast {
namespace Stats {

/// One fighter's numbers for one round, or for the whole fight.
struct Fighter {
    int sigLanded = 0;
    int sigThrown = 0;
    int totalLanded = 0;
    int takedownsLanded = 0;
    int takedownsAttempted = 0;
    int knockedDown = 0; ///< id 1683 for this fighter: how often he was dropped
    int headHealth = 100;
    int overallHealth = 100;
};

/// The counters the momentum meter watches (Phase 3), for one fighter: the fight totals the game keeps
/// at round 0. `knockdowns` and `stuns` are credited to the fighter who caused them, i.e. they are the
/// opponent's own cells.
struct MomentumCounters {
    int strikesLanded = 0;
    int headStrikesLanded = 0;
    int takedownsLanded = 0;
    int knockdowns = 0;
    int stuns = 0;
};

/// Home is slot 1: the left column and red corner, like the game's own stats page. Away is slot 0,
/// the right column and blue corner.
struct Snapshot {
    Fighter home;
    Fighter away;
};

enum class KnockdownKind {
    None,
    HomeDropped, ///< the away fighter scored, home is on the canvas
    AwayDropped,
};

struct Knockdown {
    KnockdownKind kind = KnockdownKind::None;
    int count = 0;             ///< how often the dropped fighter went down in the whole fight
    std::uint64_t serial = 0;  ///< rises once per knockdown, so the card shows each one once
};

/// Reads the mod's own ini state. Called from Broadcast::Init().
void Init();

/// Per frame: keeps the cached cells fresh, re-captures when the fight changes, and polls the
/// knockdown counter. Cheap enough to call every frame.
void Tick();

/// Phase 4 discovery (F11): dump every array the stat store holds for the judge scorecard ids, so
/// the layout is read off the game rather than guessed. Appends to broadcast_scorecard.log.
void DumpScorecard();

/// The game's own running cards, read from FIGHTER_SCORE_CARD (id 7573).
/// `score[slot][round-1]` is the sum of all three judges for that round: 30 when they all gave it,
/// 27 when they all gave it away, 29/28 when they split. Slot 1 is home (red), slot 0 away (blue).
struct Scorecard {
    bool valid = false;
    int rounds = 0;         ///< how many rounds already have a card
    int total[2] = {0, 0};  ///< running total, indexed by slot
    int score[2][5] = {};   ///< per round, indexed by slot then round-1
};

/// Reads the cards out of the stat store. False when the cache is cold or the id is absent, which
/// is what happens outside a fight and before the first bell.
bool ReadScorecard(Scorecard& out);

/// True in a live fight: the round is 1..5 and the clock is a plausible round clock.
bool InFight();

/// True when the fight is stopped: paused, between rounds, or the clock has not moved for a second.
bool Paused();

/// The round being fought, 0 when there is none.
int Round();

/// The stats of the round before the current one. False unless a round 2+ fight is running.
bool PreviousRound(Snapshot& out);

/// The fight so far: the game's own total, or the sum of the rounds when that total is not there.
bool FightSoFar(Snapshot& out);

/// The most recent knockdown, consumed once: serial changes only when a counter really rose.
Knockdown PollKnockdown();

/// The momentum meter's counters for one fighter (slot 0 away, 1 home), read from the fight total.
/// False when the cache is empty or the store does not answer.
bool ReadMomentum(int slot, MomentumCounters& out);

/// True once the cells for this fight are cached. The card draws only when it is.
bool Ready();

/// Why the stats are not there, for the log; nullptr when they are.
const char* Status();

/// Diagnostics (dump_stats): logs the raw store cells behind every card row, so a card can be lined
/// up with the game's own statistics page at the same instant.
void Dump();

} // namespace Stats
} // namespace Broadcast
