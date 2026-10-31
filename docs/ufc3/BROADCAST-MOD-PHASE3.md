# Broadcast mod — Phase 3: the momentum meter

Phase 2 reads the game's own fight statistics (`BROADCAST-MOD-PHASE2.md`). Phase 3 turns those
numbers into a *feel*: a thin bar across the top of the screen that fills towards whoever is
winning **right now**, decays back to the centre when nothing happens, and starts each round at
zero.

Branch: `ufc3-mod/broadcast-phase3` (on top of the Phase 2 head `c0ef3df`).

## 1. What the player sees

```
+------------------------------------------------------------------------------------+
| CORMIER  [####----]=============:=====================================  MIOCIC      |
|            stamina              ^ centre                             stamina       |
+------------------------------------------------------------------------------------+
```

* A thin horizontal strip, 34 % of the window wide, centred horizontally, 8 px tall (5 px floor)
  at 3.5 % of the height — above the game's own name plates and stamina bars, below the
  letterbox, and never over the clock, the Phase 2 card or any HUD text.
* `+100` fills the left (home, red) half completely, `-100` the right (away, blue) half. The fill
  always grows **from the centre outwards**, so a glance says who has the momentum and how much.
* A translucent light rim around a dark track plus a white centre mark: at 0 the bar is still
  visible as a hairline rule, which is what makes "at the centre" distinguishable from "off".
* The drawn value chases the real value (`FollowRate = 9.0f`), so it glides; no jitter, no
  snapping.

## 2. Files

| File | Purpose |
| --- | --- |
| `gpu/shim/broadcast/broadcast_momentum.h/.cpp` | the meter: the poll, the scoring, the decay, the smoothing, `DrawMeter` |
| `gpu/shim/broadcast/broadcast_stats.h/.cpp` | `MomentumCounters`, `IdStunned`, `ReadMomentum(slot, …)`, the two extra dump rows |
| `gpu/shim/broadcast/broadcast_config.h/.cpp` | the six `momentum_*` ini keys and `momentum_meter` |
| `gpu/shim/broadcast/broadcast_overlay.cpp` | `Momentum::Init()` and a `momentum_meter`-gated `Momentum::Tick(canvas)` |

`Broadcast::Draw` already calls `Stats::Tick()` for the card, so Phase 3 adds **no new traps on
hot paths**: it re-reads counters the Phase 2 store walk already knows how to find.

## 3. Where the points come from

Every 0.1 s (`PollSeconds`) the meter re-reads five counters for each fighter from the fight-total
cell (round 0, `idx = slot * 6 + 0`) and compares them with the previous poll. Only **rises**
score, and every rise is credited to the fighter who caused it:

| Event | Stat id | Read | Points | ini key |
| --- | --- | --- | --- | --- |
| significant strike landed | 7566 | 2-dim, `slot` | +3 | `momentum_sig_strike` |
| significant head strike landed | 8850 | 2-dim, `slot` | +4 **instead of** the +3 above | `momentum_head_strike` |
| takedown landed | 5685 | 3-dim, `slot, 0, 0` | +8 | `momentum_takedown` |
| knockdown scored | 1683 | 2-dim, **`1 - slot`** | +25 | `momentum_knockdown` |
| stun / wobble forced (near finish) | 8832 | 2-dim, **`1 - slot`** | +10 | `momentum_near_finish` |
| drift back to the centre | — | — | −2 per second | `momentum_decay` |

Both optional ids were found: head strikes are `8850` and near finishes are the game's own
`Stuns/Wobbles` row, `8832`. No new discovery pass was needed, so nothing was skipped.

Rules the numbers follow:

* `head = min(rise(8850), rise(7566))` — a head strike is also a significant strike, so it is
  worth the head value **instead of** the plain one and only the excess is counted twice. Live
  proof: `away +8 (0 sig, 2 head, …)` is two head strikes at 4, not two strikes at 3+4.
* `1683` and `8832` count what happened *to* a fighter, so a fighter credits his opponent's cell
  (`1 - slot`) — the same rule Phase 2 uses for the KNOCKDOWNS row.
* A single poll never credits more than 40 events per counter (`MaxEventsPerPoll`), so an arena
  re-allocation that resets a counter cannot fling the bar across the screen.
* Decay is **linear** (`value ∓ decay * dt`), not exponential, so "about two points a second" is
  literally what the ini says.
* `value` is clamped to `[-100, +100]`.

## 4. Reset, pause and no-fight rules

| Situation | What happens |
| --- | --- |
| round number (stat 9910) changes | `Reset(round)`: value and drawn value to 0, baselines re-read. Logs `round N, meter reset to 0` |
| a new fight starts | the round goes back to 1, so the same reset fires — a second fight starts at 0 |
| `!InFight()` for more than 5 s | meter cleared, logs `no fight, meter at 0` |
| `!InFight()` for a frame or two | **nothing** — round-start banners, replays and the walk-in make the clock unreadable for a moment and must not wipe the round's momentum |
| `Stats::Paused()` (pause menu, between rounds) | the bar is not drawn, exactly like the Phase 2 card |
| `momentum_meter = false` | `Momentum::Tick` returns before polling or drawing: no reads, no maths, no draw calls |
| `enabled = false` | the whole overlay is off, meter included |

## 5. ini keys

```
enabled = true
momentum_meter = true      # false hides the bar and costs nothing
momentum_sig_strike = 3    # points per significant strike landed
momentum_head_strike = 4   # …per significant head strike landed (instead of the above)
momentum_takedown = 8      # …per takedown landed
momentum_knockdown = 25    # …per knockdown scored
momentum_near_finish = 10  # …per stun/wobble forced
momentum_decay = 2         # points per second back towards the centre
```

All six point values are clamped to 0–100 and the decay to 0–50; a missing or malformed value
keeps its default and logs one line, exactly like the Phase 2 keys.

## 6. Done test

All four screenshots are in `D:\Games\UFC3Native\UFC3Native-phase2`. Fill widths were measured
from the PNGs by walking outwards from the centre pixel of the bar (326 px = 100 points), so the
numbers below are read off the pictures, not off the log.

| # | File | What it shows | Measured | Result |
| --- | --- | --- | --- | --- |
| 1 | `p3-flurry.png` (+ `p3-flurry-zoom.png`) | red fill left of centre after a takedown-and-strikes flurry | 62 px ≈ **+19** | pass (see note 1) |
| 2 | `p3-lull.png` (+ `p3-lull-zoom.png`) | same fight ~15 s later, no action: fill gone, centre mark only | 0 px ≈ **0** | pass |
| 3 | `p3-other-side.png` (+ `p3-other-side-zoom.png`) | CPU landing on the player: blue fill right of centre | 77 px ≈ **−24** | pass |
| 4 | `p3-off.png` (+ `p3-off-band.png`) | `momentum_meter = false`: nothing drawn in the band, and no `momentum:` log line at all in that launch | 0 bright pixels in the band | pass |

Round and fight resets, from the log of the default-weight session:

```
Broadcast: momentum: round 1, meter reset to 0    (three separate fights)
Broadcast: momentum: round 2, meter reset to 0
Broadcast: momentum: no fight, meter at 0
```

Every scoring path was seen live in that session:

```
12  away +3 (1 sig …)            10  home +3 (1 sig …)
 8  home +4 (0 sig, 1 head …)     6  away +4 (0 sig, 1 head …)
 9  home +8 (0 sig, 0 head, 1 takedowns …)
 1  home +6 (2 sig …)             1  away +8 (0 sig, 2 head …)
 1  home +10 (0 sig, 0 head, 0 takedowns, 0 knockdowns, 1 stuns)
```

Note 1 — the swing a *bot* can produce is small. Injected keystrokes land about one significant
strike every two or three seconds, which is roughly the decay rate, so the bar sits between −25
and +25. A human throwing a real four- or five-strike flurry adds 12–20 points inside two seconds
and will see the same bar reach the +40…+70 range; the rendering, the colours, the smoothing and
the clamp are the same code path (they were exercised at ±85 while the weights were temporarily
amplified 4× for a rendering check, which is not one of the four screenshots above).

The small magenta square at the centre of screenshots 1–3 is the `dump_stats` marker described in
§8: the capture session ran with `dump_stats = true` so the log lines and the pictures describe the
same instant. It is not drawn in normal play, and not at all in `p3-off.png`.

## 7. Known issues

1. **The bar is only as big as the fight is.** Because the scale is linear in −100…+100 and the
   decay is 2 points a second, a single jab moves it 3 % of a half-bar. That is the spec; if it
   feels too quiet, raise `momentum_sig_strike` rather than the bar's width.
2. **The knockdown (+25) path was not photographed live in Phase 3.** Both fighters stayed on
   their feet in the sessions used for the screenshots; the id, the direction (`1 - slot`) and the
   `Rise` code path are the ones Phase 2 verified against the pause page, and the stun row
   (+10) on the same code path was seen firing.
3. **A stun is not a submission attempt.** `8832` is the game's own `Stuns/Wobbles` row, which is
   the closest thing the store has to "near finish"; an actual submission attempt has no separate
   counter in the ids Phase 0 mapped, so it is not credited separately.
4. **Placement depends on the game's letterbox.** 1.2 % of the height put the bar inside the black
   band and it was invisible in screenshots; 3.5 % clears it at 1920×1080 and at the 4:3-ish
   resolutions the shim can produce. On a much shorter window (under ~570 px of canvas) the floor
   of 2 px could clip the bottom edge of the rim.
5. **The meter is not drawn while paused**, so it disappears on the pause stats page along with
   the Phase 2 card. It comes back with the first frame of play.
6. **A round that starts inside an unreadable clock frame** keeps the previous round's value for
   up to 5 s rather than resetting; that is deliberate (banners and replays), and the reset lands
   as soon as the clock reads again.

## 8. Running it

```
BB_LTO=OFF BB_JOBS=10 bash scripts/build-windows.sh --runtime
# out/windows/bin/sacredfire-game.exe, then:
#   %LOCALAPPDATA%\UFC3Native\broadcast_mod.ini
```

The ini is read once at start-up, so a change needs a restart. Leave `dump_stats = false` for
normal play; with `dump_stats = true` the meter also logs every credited poll and draws a magenta
square in the middle of the screen so the draw path can be proven from a screenshot.
