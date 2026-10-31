# Broadcast mod — Phase 2: the stat pop-up card

Phase 0 proved the fight statistics can be read out of a running game
(`BROADCAST-MOD-PHASE0.md` §3.4.1). Phase 1 proved we can draw (`BROADCAST-MOD-PHASE1.md`).
Phase 2 joins them: a bottom-centre broadcast card that shows both fighters' numbers, and
the numbers are read from the game's own statistics store, not guessed.

Branch: `ufc3-mod/broadcast-phase2` (on top of the Phase 1 commit `ba99304`).

## 1. What the player sees

```
+------------------------------------------------------------------------------------+
| RED                       ROUND 1 STATS                                    BLUE    |
|  12/28  (43%)          Significant strikes          8/19  (42%)                     |
|    19                  Total strikes landed            14                           |
|   1/2                  Takedowns                       0/0                          |
|     0                  Knockdowns                      1                            |
| [======   ]                       Head                [========  ]                  |
| [=======  ]                      Overall              [========  ]                  |
+------------------------------------------------------------------------------------+
```

* Bottom centre, 62 % of the window wide, its top edge at 83 % of the height, scaled by
  `max(height / 1080, 0.75) * ui_scale`. Home (red, the player's fighter) on the left,
  away (blue) on the right — the way the game's own pause-menu page lays them out.
* It fades in and out over 0.5 s at both ends and is never drawn over the pause menu, the
  between-rounds screen or any other menu.
* Rows, top to bottom: significant strikes landed/thrown with the percentage, total strikes
  landed, takedowns landed/attempted, knockdowns, then two damage bars — head health
  (stat id 8851) and overall health (id 8927), the Phase 2 default.

Three ways it appears:

| When | What it shows | Key |
| --- | --- | --- |
| `popup_delay` seconds into round 2 or later, once per round | the round that just ended, read from that round's own cells | `popup_seconds` |
| the moment a fighter's `FIGHTER_KNOCKED_DOWN_COUNT` goes up | a short `KNOCKDOWN` card, the count and who went down | `knockdown_seconds` |
| the card hotkey (default F7) | the fight so far, from the game's own fight-total cell | `popup_seconds` |

## 2. Files

| File | Purpose |
| --- | --- |
| `gpu/shim/broadcast/broadcast_stats.h/.cpp` | the capture, the stat store walk, the per-fight cache, `ReadFighter`, `FightSoFar`, `PreviousRound`, the knockdown watch, `Dump` (support aid) |
| `gpu/shim/broadcast/broadcast_card.h/.cpp` | the card itself: the three modes, the layout, the rows, the bars, the hotkey |
| `gpu/shim/broadcast/broadcast_config.h/.cpp` | the new ini keys |
| `gpu/shim/broadcast/broadcast_overlay.h/.cpp` | `Canvas`, `Rgba`, `MakeCanvas`, `DrawText`, `DrawRect`, `DrawBar` promoted to a public drawing layer |
| `gpu/shim/bbport_overlay.cpp` | no change beyond Phase 1: `Broadcast::Init`, `HandleKey`, `Visible`, `Draw` |

The new sources are picked up by the existing `shim/*/*.cpp` glob in `gpu/CMakeLists.txt`.

`Broadcast::Draw` calls `Stats::Tick()` first (it needs a frame to watch the clock, the bell
and the knockdown cell) and then `Card::Tick(canvas)`. `Card::HandleKey` runs on key-down
before the game sees the key; F7 is not a key the game uses.

## 3. How the numbers are read

### 3.1 The store and the capture

Every statistic the game keeps lives in one hash table ("the stat store"). The game reaches
it two ways: a fast scalar read (`0xb92c80`, `push rbx`, id in `esi`) and a general getter
that loads one cell (`0x1acb77` integer, `0x1acbd7` float, id in `r12`). Phase 0's probe
already traps those sites.

Phase 2 keeps the same idea but bounded and self-closing: `Arm()` writes an `int3` at the
two load sites, the vectored handler records *which id was read from which base with which
byte offset*, and two seconds later the sites are restored. No patch is ever left behind
while the fight is being played.

One capture is enough to learn the whole store description, because the store pointer
(`rdi`) plus the id is all we need to walk the table ourselves afterwards:

```
store + 0x10 -> bucket array, store + 0x18 -> bucket count
bucket = id % bucketCount -> chain of nodes {id, descriptor}
descriptor + 0x10 = element count, + 0x18 = value array, + 0x8 = stride array
```

`Lookup(store, id, dims)` does exactly that on every use, so a re-allocated arena is noticed
instead of read stale. `IndexOf` then computes the element the game itself would compute —
the dot product of `{slot, round, 0}` with the strides — so nothing about the layout is
hardcoded beyond "slot 0 is away, slot 1 is home, round 0 is the fight total".

### 3.2 When a capture is taken

* Cache invalid (start of a fight, or the game went back to its menus): capture at once.
* The bell rang (`ROUND_TIME_REMAINING` was seen above 5 s and then dropped to ≤ 2 s): capture
  again, because the next fight has its own arena.
* Health unknown: see §3.4.

`Rebuild()` trusts nothing it has not validated: round number 1..5 and a clock of 0..900 s,
otherwise the capture was a menu and is thrown away. That also stops the Phase 1 problem of
latching menu cells.

### 3.3 The statistics, and why some rows read a sub-slot

| Row | id | Shape in the store | What the card reads |
| --- | --- | --- | --- |
| significant strikes landed | 7566 | 2-dim, `6,1` | `[slot][round]` |
| significant strikes thrown | 9567 | 2-dim, `6,1` | `[slot][round]` |
| total strikes landed | 3152 | 3-dim, `90,15,3` | last sub-slot of the row — the game's own row total |
| takedowns landed | 5685 | 3-dim, `42,7,1` | first sub-slot of the row |
| takedowns attempted | 9343 | 2-dim, `6,1` | `[slot][round]` |
| knockdowns | 1683 | 2-dim, `6,1` | `[slot][round]`, and the row uses the **opponent's** cell (see §6) |
| head health | 8851 | 1-dim | base +4 away, +8 home, float |
| overall health | 8927 | 1-dim | base +4 away, +8 home, int |

`3152` decodes as five sub-slots of three numbers each (`v v 0`), the fifth being the row
total the game prints: measured four times independently (`10+2+11+0 = 23`, `22+3+5+0 = 30`,
`21+3+2+1 = 27`, `10+3+1+1 = 15`) and each time equal to the pause page's number. So the card
reads the game's own total instead of adding anything up.

`5685`'s row is `4 0 2 0 0 0 0` when the page says "4 landed / 4 attempted", so the landed
count is sub-slot 0, not the row total (the rest of the row is not a strike-type split).

Per-round cells are read as stored, with no subtraction. Round 1's cells freeze when round 2
starts and the new round writes its own cells from zero, so:

* "the previous round" = that round's cells;
* "the fight so far" = the game's own round-0 total cell, which is what the pause page shows.

### 3.4 Health, the slow one

Head (8851) and overall (8927) health are the only rows that come through the general
getter's **float** load, and the game only reads them when it draws a fighter's damage — the
pause menu does not. Phase 2 therefore takes a few short, rate-limited (8 s apart, at most
12 per fight) captures during the fight until one of them catches that read. The log says
which happened:

```
Broadcast: stats cached (…, with health) 3152:ok n=180 s=90,15 …
```

Decoding: try the cell as a float first, but only in `[0.5, 200]` — a plain small integer
reinterpreted as a float is a denormal and lands nowhere near that window — otherwise take it
as an integer in `[0, 200]`. Clamp to 100. An unwritten cell reads -1 and is ignored.
If health is still unknown, the bars draw full and the card keeps working.

Each of the two cells is hunted on its own and health that a capture of *this* fight already
found is carried across later captures (a bell or a probe re-capture must not throw away a
number that is still valid), so the probes stop as soon as both cells are known instead of
running for the whole fight.

## 4. `broadcast_mod.ini`

Everything is behind a switch; the file is written with these defaults on first start and
missing keys fall back to them, so an old ini still works.

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` | master switch for the whole mod |
| `stat_popups` | `true` | this phase: the stat card |
| `momentum_meter` | `true` | reserved for Phase 3 (drawn nowhere yet) |
| `scorecard_reveal` | `true` | reserved for Phase 3 |
| `ui_scale` | `1.0` | 0.5 .. 3.0 |
| `toggle_key` | `F8` | master on/off, works in and out of a fight |
| `card_key` | `F7` | show the fight so far now |
| `popup_seconds` | `5` | how long a card stays up |
| `popup_delay` | `15` | seconds into a round before the previous round is shown |
| `knockdown_seconds` | `3` | how long the knockdown card stays up |
| `debug_card` | `false` | Phase 1's "Broadcast mod loaded" panel |
| `dump_stats` | `false` | support aid: pressing the card key also logs the raw cells |

Nothing here can crash the game. A missing store, an unreadable cell, a capture that saw a
menu: each one just leaves the card hidden and logs one line.

## 5. Verification

The method: pause the fight on the game's own statistics page and press the card key there,
so the dump and the page describe the same instant (`dump_stats = true`). Every pair below is
one such instant, from `Cormier (home) vs Miocic (away)`, and the card dump is
`Broadcast: dump card: home/away …`.

Round 1, first pair (`20261031_183057.log`):

| Row | game page | card | |
| --- | --- | --- | --- |
| home significant | 0 / 0 | 0 / 0 | ok |
| home total landed | 0 | 0 | ok |
| home takedowns | 0 / 0 | 0 / 0 | ok |
| home knockdowns | 0 | 0 | ok |
| away significant | 7 / 13 | 7 / 15 | thrown +2 |
| away total landed | 22 | 22 | ok |
| away takedowns | 1 / 2 | 1 / 2 | ok |
| away knockdowns | 0 | 0 | ok |

Round 1, second pair, 35 s later:

| Row | game page | card | |
| --- | --- | --- | --- |
| home significant / total / TD / KD | 0/0, 0, 0/0, 0 | 0/0, 0, 0/0, 0 | ok |
| away significant | 9 / 16 | 9 / 18 | thrown +2 |
| away total landed | 28 | 28 | ok |
| away takedowns | 2 / 3 | 2 / 3 | ok |
| away knockdowns | 0 | 0 | ok |

Round 1, third pair (`20261031_184505.log`), a round fought entirely at range:

| Row | game page | card | |
| --- | --- | --- | --- |
| home significant / total / TD / KD | 0/0, 1, 0/0, 0 | 0/0, 1, 0/0, 0 | ok |
| away significant | 3 / 8 | 3 / 8 | ok |
| away total landed | 9 | 9 | ok |
| away takedowns / knockdowns | 0/0, 0 | 0/0, 0 | ok |

Fourth pair (`20261031_190206.log`), after a fighter had been knocked down in an earlier
fight of the same session: home `0/0, 1, 0/0, 0` and away `4/6, 7, 0/0, 0` on the page, the
same numbers on the card — every row, including significant strikes thrown, matched.

Health in that pair: store floats `head [1]=100.00 [2]=93.94`, `overall [1]=98 [2]=97`, card
`head=94 overall=97` for home and `head=100 overall=98` for away.

Health at the second pair's instant: store floats `head [1]=100.00 [2]=84.79`,
`overall [1]=100 [2]=94` for away/home, and the card printed `head=85 overall=94` for the
home fighter and `head=100 overall=100` for the away fighter — the same cells, decoded.

Earlier same-instant pairs agreed the same way, including a fight where **every** row matched
exactly (`cmp7-menu.png` with `20261031_173213.log`: total landed 30/27, thrown 53/34,
significant 9/9 and 6/6, takedowns 4, knockdowns 1/0).

Rendered output was checked on screen as well: the scheduled round card
(`Broadcast: card: round 1 stats, 15.0 s into round 2`) and the on-demand card both appear
bottom-centre at 1080p and at a 1920-wide window without covering the round clock, the names
or the health bars.

## 6. Decisions worth knowing

* **Ground truth is the capture, not the screen.** Stylised digits OCR badly; the store's own
  cells and the pause page's printed numbers do not.
* **The knockdown row uses the opponent's cell.** `1683` is "times *this* fighter was knocked
  down" (the pause page's KNOCKDOWNS row for a fighter is the other fighter's `1683`), so the
  card reads `opponentC` for each column and the pop-up fires when a fighter's own `1683` rises.
* **Names are `RED`/`BLUE`.** The fighter names are not in the stat store; the card therefore
  uses the same placeholders the corner-select screen does.
* **The `dump_stats` dump is a support aid, not a feature.** It is off by default and only
  fires from the card key.
* **No second renderer, no input remap.** The card rides the overlay frame the port already
  draws and reads the keyboard through the existing `HandleKey` hook (`SDLK` codes).

## 7. Known issues

1. **Significant strikes thrown can read 2 high** (and total strikes thrown 3–4 high) after a
   grappling exchange. In the first two pairs the store said 15 and 18 where the page said 13
   and 16, and 45 where the page said 41; landed, takedowns, knockdowns, health and the
   total-strikes *landed* row were exact. The offset appeared in a ground-and-pound round and
   stayed constant while the fight went on, which points at the store counting strike
   *initiations* that the page drops when the strike is interrupted (stuffed by a takedown,
   cancelled by a grapple). The two rounds fought at range (third and fourth pairs) matched
   thrown exactly, so this is a small, situational skew rather than a mapping error. It moves
   the percentage in the significant-strikes row by a point or two.
2. **A knockdown card was seen firing once** (`Broadcast: knockdown, home fighter down (1)`)
   but not photographed with the new layout, and the KNOCKDOWNS row has only been compared
   with the page while it was 0/0. The credit direction it uses (a fighter's column reads the
   opponent's 1683) is the one measured in Phase 2 stage B against the page.
3. **Health can take up to ~8 s of fighting to appear.** Until the first capture catches the
   HUD reading 8851/8927 the bars draw full. The card never blocks or crashes; the log line
   `with health` / `without health` says which state it is in.
4. **Cards are not drawn while paused** on purpose (they would cover the page they are
   describing), so the F7 card is only visible during play.
5. **The card shows "fight so far" from the game's round-0 total cell.** If the game itself has
   not updated that cell yet (it lags a frame or two behind the live HUD), the card shows the
   same slightly-old number the pause page shows — by design, since matching that page is the
   acceptance test.
6. **A capture that lands outside a fight is retried every 8 s** and logs
   `the captured clock cells do not look like a round (round 0, remaining 0)`. That is the
   walk-in and the menus; nothing is latched and the first capture inside the fight takes over.

## 8. Running it

```
BB_LTO=OFF BB_JOBS=8 bash scripts/build-windows.sh --runtime
# out/windows/bin/sacredfire-game.exe, then:
#   %LOCALAPPDATA%\UFC3Native\broadcast_mod.ini
```

Test build used here: `ufc3-v0.1.14-11-g4da3f1f-dirty`. Press F8 to toggle the mod, F7 to
show the fight so far, and set `dump_stats = true` to log the raw cells alongside it.
