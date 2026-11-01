// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 4. The judges' scorecard reveal.
// See broadcast_scorecard.cpp.
#pragma once

#include <cstdint>

namespace Broadcast {

struct Canvas;

namespace Scorecard {

/// Nothing to load beyond the shared config. Called once from Broadcast::Init().
void Init();

/// Per frame, after the stats module ticked. Reveals the cards at each bell and draws them.
void Tick(const Canvas& canvas);

} // namespace Scorecard
} // namespace Broadcast
