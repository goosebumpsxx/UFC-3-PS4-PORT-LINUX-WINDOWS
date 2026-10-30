// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 0 discovery probe. See broadcast_probe.cpp.
#pragma once

namespace BroadcastProbe {

/// BB_BROADCAST_PROBE=1: the probe is armed and F9 scans.
bool Enabled();

/// Starts a scan on its own thread (a scan takes a few seconds; the caller keeps rendering).
void Request();

} // namespace BroadcastProbe
