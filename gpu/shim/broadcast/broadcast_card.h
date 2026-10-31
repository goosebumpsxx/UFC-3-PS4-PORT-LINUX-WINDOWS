// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 2. The stat card: previous-round, knockdown and on-demand.
// See broadcast_card.cpp.
#pragma once

#include <cstdint>

namespace Broadcast {

struct Canvas;

namespace Card {

/// Reads the ini values that only the card uses. Called once from Broadcast::Init().
void Init();

/// Per frame, after the stats module ticked. Decides what (if anything) is up and draws it.
void Tick(const Canvas& canvas);

/// The on-demand key (`card_key`, F7 by default). Returns true when it was ours; the caller never
/// consumes it, so the game keeps seeing the key.
bool HandleKey(std::uint32_t key);

} // namespace Card
} // namespace Broadcast
