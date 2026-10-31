# Broadcast mod — Phase 3: the momentum meter, and the numbers the game's bar does not show

Phase 2 reads the game's own fight statistics (`BROADCAST-MOD-PHASE2.md`). Phase 3 turns those
numbers into a *feel*: a long thin bar across the top of the screen that fills towards whoever is
winning **right now**, plus the numbers a broadcast would put around it — what each fighter has
earned this round, who leads it, what just scored and who has taken the rounds so far.

The game already draws a **short ticked momentum bar of its own**, about 4.6 % of the height down
(just under ours). Ours is the long bar above it: the same 34 % of the width, but with its own
scoring that moves it further, and with text.

Branch: `ufc3-mod/broadcast-phase3` (on top of the Phase 2 head `c0ef3df`).

> **Reworked once.** The first cut of Phase 3 was sized so quietly that its fill barely moved, and
> the four screenshots that passed its done test were in fact measuring the *game's* ticked bar, not
> ours. This version (the one documented here) doubles the point values, halves the decay, adds the
> round scoring and the text, and lifts the bar clear of the game's. Its done test measures our own
> bar only.

## 1. What the player sees

```
                        ROUND 2 EDGE: CORMIER +13                  <- who leads the round
   CORMIER 34  [============###|-------------------]  21 MIOCIC   <- round points, corner colour
               +8 TAKEDOWN CORMIER                                  <- last event, 2 s
                     R1 CORMIER  R2 MIOCIC                          <- rounds won so far
                     | | | | | | | | | | | |                        <- the game's own ticked bar
```

* **The bar**: 34 % of the window wide, centred, at 2.8 % of the height (10 px tall, 6 px floor).
  `+100` fills the left half (home, red), `-100` the right half (away, blue); the fill grows **from
  the centre outwards** towards the corner of whoever is winning. A light rim around a dark track
  and a white centre mark keep "at the centre" distinguishable from "off".
* **The round score** at each end, in the corner's colour: `CORMIER 34` … `21 MIOCIC`. These are the
  points each fighter has *earned this round* — no decay, so they only ever go up until the round
  resets them.
* **The leader line** above the bar: `ROUND 2 EDGE: CORMIER +13`, or `ROUND 2: EVEN` when the round
  is within `momentum_even_margin` points.
* **The event tag** for two seconds under the scoring side: `+8 TAKEDOWN CORMIER`,
  `+16 STUN MIOCIC`, `+40 KNOCKDOWN CORMIER`, in that fighter's colour.
* **The slate** of rounds decided so far under the bar: `R1 CORMIER  R2 MIOCIC`, with `EVEN` for a
  round won by nobody inside the margin. It resets with the fight.
* Everything is placed so it clears the game's own ticked bar (which is ~10 px lower at 1080p), the
  game's name plates and stamina bars, the round clock and the Phase 2 card. Nothing is drawn while
  the fight is paused.

## 2. Files

| File | Purpose |
| --- | --- |
| `gpu/shim/broadcast/broadcast_momentum.h/.cpp` | the meter: the poll, the scoring, the round accounting, the decay, the smoothing, `DrawMeter` |
| `gpu/shim/broadcast/broadcast_stats.h/.cpp` | `MomentumCounters`, `IdStunned`, `ReadMomentum(slot, …)`, the two extra dump rows |
| `gpu/shim/broadcast/broadcast_config.h/.cpp` | `momentum_meter`, the six point/decay keys, `momentum_even_margin`, the two name keys |
| `gpu/shim/broadcast/broadcast_overlay.h/.cpp` | `DrawTextAt` / `TextWidthAt` / `TextLineHeightAt` (text of any size, aligned), `Momentum::Init()`, the gated `Momentum::Tick(canvas)` |

`Broadcast::Draw` already calls `Stats::Tick()` for the card, so Phase 3 adds **no new traps on hot
paths**: it re-reads counters the Phase 2 store walk already knows how to find.

## 3. Where the points come from

Every 0.1 s (`PollSeconds`) the meter re-reads five counters for each fighter from the fight-total
cell (round 0, `idx = slot * 6 + 0`) and compares them with the previous poll. Only **rises** score,
and every rise is credited to the fighter who caused it:

| Event | Stat id | Read | Points | ini key |
| --- | --- | --- | --- | --- |
| significant strike landed | 7566 | 2-dim, `slot` | +8 | `momentum_sig_strike` |
| significant head strike landed | 8850 | 2-dim, `slot` | +10 **instead of** the +8 above | `momentum_head_strike` |
| takedown landed | 5685 | 3-dim, `slot, 0, 0` | +18 | `momentum_takedown` |
| knockdown scored | 1683 | 2-dim, **`1 - slot`** | +40 | `momentum_knockdown` |
| stun / wobble forced (near finish) | 8832 | 2-dim, **`1 - slot`** | +16 | `momentum_near_finish` |
| drift back to the centre | — | — | −1 per second | `momentum_decay` |

Both optional ids were found: head strikes are `8850` and near finishes are the game's own
`Stuns/Wobbles` row, `8832`. No new discovery pass was needed, so nothing was skipped.

Rules the numbers follow:

* `head = min(rise(8850), rise(7566))` — a head strike is also a significant strike, so it is worth
  the head value **instead of** the plain one. Live proof: `away +10 (0 sig, 1 head, …)` is one head
  strike at 10, and `away +18 (1 sig, 1 head, …)` is a head strike plus a body strike (10 + 8).
* `1683` and `8832` count what happened *to* a fighter, so a fighter credits his opponent's cell
  (`1 - slot`) — the same rule Phase 2 uses for the KNOCKDOWNS row.
* A single poll never credits more than 40 events per counter (`MaxEventsPerPoll`), so an arena
  re-allocation that resets a counter cannot fling the bar across the screen.
* Decay is **linear** (`value ∓ decay * dt`), so "one point a second" is literally what the ini says.
* The bar's `value` is the running net of both fighters' credits, clamped to `[-100, +100]`, and the
  drawn value chases it (`FollowRate = 9.0f`) so the fill glides instead of snapping.
* Points are counted **per round**: `roundPoints[home/away]` add up without decay and are cut to zero
  when the round changes, which is what the two end labels and the leader line show.

## 4. Rounds, resets and pauses

| Situation | What happens |
| --- | --- |
| round number (stat 9910) changes | `Reset(round)`: closes the round that ended (its winner goes on the slate), value and drawn value to 0, round points to 0, baselines re-read. Logs `round N, meter reset to 0` |
| the round number goes **backwards** | a new fight: the rounds-won slate is cleared as well |
| `!InFight()` for more than 5 s | meter cleared, logs `no fight, meter at 0` |
| `!InFight()` for a frame or two | **nothing** — round-start banners, replays and the walk-in make the clock unreadable for a moment and must not wipe the round's momentum |
| `Stats::Paused()` (pause menu, between rounds) | nothing is drawn, exactly like the Phase 2 card |
| `momentum_meter = false` | `Momentum::Tick` returns before polling or drawing: no reads, no maths, no draw calls |
| `enabled = false` | the whole overlay is off, meter included |

A round is won by whoever leads it by **more than** `momentum_even_margin` points; anything inside
the margin is `EVEN` on the slate and `ROUND n: EVEN` on the leader line.

## 5. ini keys

```
enabled = true
momentum_meter = true        # false hides the bar, the scores, the leader line, the tag and the slate
momentum_sig_strike = 8      # points per significant strike landed
momentum_head_strike = 10    # …per significant head strike landed (instead of the above)
momentum_takedown = 18       # …per takedown landed
momentum_knockdown = 40      # …per knockdown scored
momentum_near_finish = 16    # …per stun/wobble forced
momentum_decay = 1           # points per second back towards the centre
momentum_even_margin = 3     # a round within this many points is EVEN
momentum_home_name =         # the names printed at the bar's ends; empty means RED and BLUE
momentum_away_name =
```

The point values are clamped to 0–100, the decay to 0–50 and the margin to 0–100; a missing or
malformed value keeps its default and logs one line, exactly like the Phase 2 keys. The store
carries no fighter names, so the names come from the ini (blank = the corners, as on the Phase 2
card); the screenshots below were taken with the two keys set.

## 6. Done test

All five screenshots are in `D:\Games\UFC3Native\UFC3Native-phase2`. Every width was measured from
the PNGs **inside our own bar** — the long 34 % strip, found by its uniform dark track — walking
outwards from the screen centre: 329 px is one half, so 3.29 px = 1 point.

| # | File | What it shows | Measured fill | Result |
| --- | --- | --- | --- | --- |
| 1 | `p3b-flurry.png` | our flurry: `CORMIER 56` … `0 MIOCIC`, `ROUND 2 EDGE: CORMIER +56`, red fill from the centre to the left, `+16 STUN CORMIER` tag, `R1 CORMIER` slate | 148 px ≈ **+45** | pass — and the game's own bar in the same frame is ~50 px, so ours moves about three times as far |
| 2 | `p3b-other.png` | the CPU ahead: `CORMIER 72` … `76 MIOCIC`, `ROUND 4 EDGE: MIOCIC +4`, blue fill from the centre to the right, `R1 CORMIER  R2 CORMIER  R3 CORMIER` slate | 30 px ≈ **−9** | pass |
| 3 | `p3b-event.png` | an event tag in the away corner's colour: `+16 STUN MIOCIC`, with `ROUND 2: EVEN`, `116` … `116 MIOCIC` and a 103 px blue fill | 103 px ≈ **−32** | pass |
| 4 | `p3b-round2.png` | first frame of a new round: both scores back at `0`, `ROUND 3: EVEN`, empty bar with the centre mark, slate `R1 CORMIER  R2 CORMIER` | 0 px ≈ **0** | pass |
| 5 | `p3b-off.png` | `momentum_meter = false`: the game's HUD and its own tick bar are there, nothing of ours is | no bar, no scores, no tag, no slate | pass |

The scoring itself, from the log of the session the screenshots came from (`20261101_001259.log`):

```
Broadcast: momentum: home +8 (1 sig, 0 head, 0 takedowns, 0 knockdowns, 0 stuns)
Broadcast: momentum: meter 22.7, round score 40-16
Broadcast: momentum: home +18 (0 sig, 0 head, 1 takedowns, 0 knockdowns, 0 stuns)
Broadcast: momentum: meter 26.0, round score 78-36
Broadcast: momentum: home +16 (0 sig, 0 head, 0 takedowns, 0 knockdowns, 1 stuns)
Broadcast: momentum: away +10 (0 sig, 1 head, 0 takedowns, 0 knockdowns, 0 stuns)
Broadcast: momentum: round 2, meter reset to 0
Broadcast: momentum: no fight, meter at 0
```

`p3b-off.png` was taken in a separate launch (`20261031_235142.log`) with `momentum_meter = false`:
**zero** `momentum:` lines in it while the mod itself logged 96 lines, i.e. the meter costs nothing
when it is off.

Screenshots 1–4 were taken with `dump_stats = true` so the log and the pictures describe the same
instant; that is the small magenta square in the middle of each of them and it is not drawn in
normal play. The two name keys were filled in with the fighters on screen; with them blank the labels
read `RED` and `BLUE`.

The four screenshots of the first Phase 3 cut (`p3-flurry.png`, `p3-lull.png`,
`p3-other-side.png`, `p3-off.png`) have been removed: they measured the game's ticked bar, which is
what this rework was about.

## 7. Known issues

1. **The bar is only as big as the fight is.** With 8/10/18/40/16 and a 1-point-a-second drift, one
   exchange puts it at ±40–45 and the bar holds that for half a minute. If that is still too quiet,
   raise the point keys — not the bar's width.
2. **The knockdown (+40) path was not photographed live.** Both fighters stayed on their feet in the
   sessions used for the screenshots; the id, the direction (`1 - slot`) and the `Rise` path are the
   ones Phase 2 verified against the pause page, and the stun row on the same path did fire
   (`+16 STUN CORMIER` in `p3b-flurry.png`).
3. **A stun is not a submission attempt.** `8832` is the game's own `Stuns/Wobbles` row, which is the
   closest thing the store has to "near finish"; an actual submission attempt has no separate counter
   among the ids Phase 0 mapped, so it is not credited separately.
4. **The bar and the bar-text can disagree, by design.** The bar is the *net* of both fighters'
   credits after decay, so it can sit at the centre while the round score still reads a lead (a
   +14 round can have a 0 bar after fifteen quiet seconds). The leader line is the round, the bar is
   the moment.
5. **Placement is tuned for 16:9.** Ours sits at 2.8 % of the height and the game's ticked bar at
   about 4.6 %; with a large `ui_scale` the tag and the slate grow towards the game's bar. The bar
   itself never overlaps it, at any scale the shim produces.
6. **Nothing is drawn while paused**, so the whole thing disappears on the pause stats page along
   with the Phase 2 card and comes back with the first frame of play.
7. **A round that starts inside an unreadable clock frame** keeps the previous round's value for up
   to 5 s rather than resetting; that is deliberate (banners and replays), and the reset lands as soon
   as the clock reads again.

## 8. Running it

```
BB_LTO=OFF BB_JOBS=10 bash scripts/build-windows.sh --runtime
# out/windows/bin/sacredfire-game.exe, then:
#   %LOCALAPPDATA%\UFC3Native\broadcast_mod.ini
```

The ini is read once at start-up, so a change needs a restart. Leave `dump_stats = false` for normal
play; with `dump_stats = true` the meter also logs every credited poll and draws a magenta square in
the middle of the screen so the draw path can be proven from a screenshot.
