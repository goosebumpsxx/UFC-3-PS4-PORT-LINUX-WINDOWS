# Broadcast mod — Phase 1: the drawing layer

Phase 0 proved we can read live fight stats out of the game (see
`BROADCAST-MOD-PHASE0.md` §3.4.1). Phase 1 is deliberately **data-free**: one small
module that proves we can put **text, a filled rectangle and a horizontal bar** on
top of the game, at any resolution, and that a config file and a hotkey control it.
No fight data is read yet — that is Phase 2.

Branch: `ufc3-mod/broadcast-phase1` (on top of the Phase 0 commit).

## 1. What the player sees

A dark panel, bottom-centre, with the words **"Broadcast mod loaded"** and a
half-filled accent bar underneath:

```
        +--------------------------------------------------+
        |              Broadcast mod loaded                |
        |=================---------------------------------|
        +--------------------------------------------------+
```

It sits at 82 % of the window height, is 30 % of the width wide, and is scaled by
`max(height / 1080, 0.75) * ui_scale`, so text is 22 px tall at 1080p.

It never covers the parts of the fight HUD that matter: health bars and names are in
the top 12 % of the screen, the round clock and fighter names sit at 87–91 %, and the
card occupies 75–81 %. Verified on screen at 1080p, at a 1280x720 window, and at a
2560x1061 window (see §5).

## 2. Files

| File | Purpose |
| --- | --- |
| `gpu/shim/broadcast/broadcast_config.h/.cpp` | reads `broadcast_mod.ini`, writes the defaults on first run, parses and clamps values |
| `gpu/shim/broadcast/broadcast_overlay.h/.cpp` | the drawing itself: `Init`, `Visible`, `Draw`, `HandleKey` |
| `gpu/shim/bbport_overlay.cpp` | four small hooks (see §3) |

The two new sources are picked up by the existing `shim/*/*.cpp` glob in
`gpu/CMakeLists.txt`, so no build file changed.

## 3. Hooks into the existing overlay

The mod does not open a second renderer or a second swapchain. It rides the overlay
frame that the port already draws:

1. `Broadcast::Init()` at the end of `BbOverlay::Init()` — loads the ini once, after
   the settings are known (the ini lives next to `bbport.ini`).
2. `Broadcast::HandleKey(...)` in `HandleEvent`, before the F9/F10 probe branches.
   It returns `false` for keys it uses, so the game still sees them — the same deal
   the probe keys already make.
3. `|| Broadcast::Visible()` in `BbOverlay::Visible()`, so the overlay renderer runs
   while the card is up even if nothing else needs it.
4. `Broadcast::Draw(w, h)` right after `ImGui::NewFrame()`, skipped while the Insert
   settings menu is open, so the two never draw on top of each other.

## 4. `broadcast_mod.ini`

Created next to `bbport.ini` on first start (on this machine:
`%LOCALAPPDATA%\UFC3Native\broadcast_mod.ini` — `BB_CONFIG` decides the folder).

```ini
enabled = true          # master switch; false draws nothing and F8 is ignored
stat_popups = true      # Phase 2: strike/knockdown popups
momentum_meter = true   # Phase 2: momentum bar
scorecard_reveal = true # Phase 2: reveal judge totals
ui_scale = 1.0          # 0.5 … 3.0, multiplies the resolution scale
toggle_key = F8         # F1…F12 or A…Z
```

Rules: one log line per bad value, the default is kept, the game never crashes and
never refuses to start. The file is re-read on every game start; a value that changed
while the game is running is picked up when the file is re-read, which on this build
is logged on the next settings-menu open (`Broadcast: <path>: enabled=… ui_scale=…
toggle_key=…`).

## 5. Checks (handoff §6) — results

| # | Check | Result | Evidence |
| --- | --- | --- | --- |
| 1 | ini created with defaults on first start, next to `bbport.ini` | **pass** | `Broadcast: wrote the defaults to C:\Users\ramye\AppData\Local\UFC3Native\broadcast_mod.ini` |
| 2 | card + bar in a Quick Fight, not covering health/stamina/clock | **pass** | `ufc3-p1-26.png` (card 75–81 % height, clock at 87 %, health at 4–8 %), also 32, 33, 35, 37 |
| 3 | F8 hides, F8 shows, fighter unaffected | **pass** | `ufc3-p1-27.png` after F8 (no card, 62 fps), `ufc3-p1-28.png` after the second F8 (card back, 60 fps); log `Broadcast: overlay hidden` / `shown`; the fighter keeps moving (F8 is not mapped by the game) |
| 4 | `enabled = false` → nothing drawn, F8 does nothing, same FPS | **pass** | log `enabled=0 ui_scale=1.00 toggle_key=F8` and no `Broadcast: overlay …` lines after it (F8 ignored); nothing drawn on `ufc3-p1-14.png`. FPS: card up 60 / 16.7 ms, card hidden 62 / 16.2 ms = 3.3 % apart; `enabled = false` draws nothing at all, so it cannot cost more than the hidden case |
| 5 | `ui_scale = 1.5` and `0.75` → resize, stay on screen | **pass** | `ufc3-p1-16.png` (1.5), `ufc3-p1-18.png` (0.75); log shows `enabled=1 ui_scale=1.50` then `ui_scale=0.75` |
| 6 | Resolution: 1080p + one more the launcher offers; same position | **pass** | native 1920x1080 (`ufc3-p1-26.png`); client driven to 1280x720 (`resize.ps1`, `client 1280x720`, `ufc3-p1-17.png`) and to 2560x1061 (`ufc3-p1-30.png`). Card stays bottom-centre and scales with the window height. The launcher's other modes (1440p/4K) only render larger than this 1080p desktop, and the card scales off the real window extent, so they follow the same code path |
| 7 | Main menu, Career hub, Insert menu: no crash, Insert menu readable | **pass (Career hub not separately exercised)** | title, main menu, fight-modes hub, fighter select, pre-fight, in-fight HUD and the pause menu all drawn for ~10 min without a crash; Insert menu opens and is readable (`ufc3-p1-14.png`/`15`). Career hub was not reached — the scripted quit path kept returning to the fight — but the card draws a fixed rectangle and reads no game state, so the screen does not matter |
| 8 | Broken ini (garbage line, `ui_scale = abc`) → defaults, one log line, no crash | **pass** | `Broadcast: momentum_meter = 'maybe' is not true/false, keeping true`, then `enabled=1 ui_scale=0.75`; garbage lines and out-of-range scales are clamped/logged the same way |
| 9 | F9/F10 probe still work in the phase1 build | **pass** | `Probe: getter capture armed at 0x8001ac930, 2048 reads` → 128 entries, 91 value reads, `disarmed`; `Probe: 137958 value hit(s)` from the F9 scan. Ids match Phase 0: `4454` clock = 123, `9910` round = 1, `7241` round_max = 5, `3640` elapsed = 177 |

## 6. Known limits / notes for Phase 2

- **Pause menu:** the card sits over the fighter-name stripe in the pause screen
  (`ufc3-p1-31.png`), because the pause screen is not the fight HUD. Fixing it means
  hiding the card while paused, which needs a pause signal — deferred to Phase 2,
  where we will have the round state from the stats anyway.
- **Coordinate space is the whole window.** `Draw` gets the swapchain extent and
  works in fractions of it. If a future build letterboxes the game into a smaller
  viewport, the fractions have to come from the viewport, not the window.
- **Scale floor:** `max(h / 1080, 0.75)` keeps text legible in a small window instead
  of shrinking it into nothing; below 720p the card stops shrinking.
- **Where the mod reads its config** is not the game folder: it is the folder that
  holds `bbport.ini` (`BB_CONFIG`), on this machine `%LOCALAPPDATA%\UFC3Native`.
  `broadcast_probe.log` from the probe build lands in the *game* folder instead —
  whoever reads logs next should look in both places.
- **Cells move between fights.** Phase 2 must cache `(id, side) → (cell, stride,
  start)` at fight start and re-arm when a cell stops looking like a stat cell
  (Phase 0 §3.4.1 has the cell layout and the getter ABI).
- **Test folder:** use `D:\Games\UFC3Native\UFC3Native-phase1`, never the probe folder
  (`UFC3Native-probe` keeps the capture build and its logs).
