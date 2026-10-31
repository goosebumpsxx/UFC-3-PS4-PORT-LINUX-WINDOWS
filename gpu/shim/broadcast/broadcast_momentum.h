// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 3. The momentum meter: who is winning right now.
// See broadcast_momentum.cpp.
#pragma once

namespace Broadcast {

struct Canvas;

namespace Momentum {

/// Nothing to load: the ini values live in the shared config. Called once from Broadcast::Init().
void Init();

/// Per frame, after the stats module ticked. Polls the counters, decays the value and draws the bar.
void Tick(const Canvas& canvas);

/// The meter as it stands: -100 (away winning) .. +100 (home winning). For the log.
float Value();

} // namespace Momentum
} // namespace Broadcast
