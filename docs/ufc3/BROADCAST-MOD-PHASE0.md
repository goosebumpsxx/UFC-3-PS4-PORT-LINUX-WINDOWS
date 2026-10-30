# Broadcast overlay mod — Phase 0 discovery

UFC 3 native port (`~/ufc3-mod`, fork at `dd559a5`). This file answers the four Phase 0 questions
and records what was measured, including the parts that are still open.

Status in one line: **rendering, config and hotkeys are solved; the fight data is a per-context
stat store reached through a virtual getter, and its container still needs one runtime capture.**

## 1. Rendering — solved, it is the easy part

The port already has a host-side ImGui overlay drawn **after** the game frame, at display
resolution, straight into the swapchain image:

| what | where |
|---|---|
| overlay module (ImGui context, Vulkan backend, everything drawn) | `gpu/shim/bbport_overlay.cpp` — `Init()` 920, `Render()` 1124, `Visible()` 1107, `HandleEvent()` 1015 |
| call site: after the game frame is blitted, before present | `gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:653-698` |
| existing corner HUD text | `FpsCounter()` `bbport_overlay.cpp:770` |
| existing timed, fading card | `Toast()` `bbport_overlay.cpp:865` (fade, queue, icons) |
| UI scale | `Render()` 1140: `scale = max(extent.height / 1080, 0.75)` applied to `ImGuiStyle` and `FontScaleMain` |
| font | DejaVu TTF compiled into the binary (`bb_font_ttf`), 18 px base |

Text = `ImGui::Text*`; filled rectangle and bar = `ImGui::GetBackgroundDrawList()->AddRectFilled()`
(track + fill), in viewport-relative coordinates. No new dependency, no new render path.

Consequence for layout: the overlay is composited **over** the finished game frame, so the game's
HUD cannot hide it — but it also cannot go behind it. The game's HUD lives in its 1920×1080 UI
target (`0x109eee0000`, see `UFC-43-ULTRAWIDE-WIDTH-CONTRACT.md`): health/stamina bars top-left and
top-right, clock bottom-centre, round timer strip at the very bottom. Everything else is free.

## 2. Config and hotkey — solved

`gpu/shim/bbport_settings.cpp` reads `bbport.ini` (`BB_CONFIG`, `key = value`, line 142 on) into
`std::atomic` values that the overlay reads per frame; the launcher and `run.sh` set the same values
through `BB_*`.

Plan: a separate `broadcast_mod.ini` next to `bbport.ini` (folder of `BB_CONFIG`) read by the new
module, so the mod's knobs stay out of the port's settings file. Hotkey: extend
`BbOverlay::HandleEvent()` (1015) — called for every SDL event from `window.cpp:283` before the game
sees it, so a toggle can return `false` and not eat game input. `F9` is free unless
`BB_PAD_RECORD` is set (`src/runtime_pad.c:856` uses it only for pad recording); F9/F10 also work
with the pad unplugged.

## 3. Fight data — where it lives

### 3.1 Host side

Nothing host-side reads fight state: a search for `stat|strike|round|score|judge|hud|overlay` in
`src/` and `gpu/shim/` only turns up frame statistics and GPU diagnostics. Fight state exists only
inside the game's own code and data, i.e. in **guest memory**, which is mapped **at the same host
addresses** (`gpu/shim/core/memory.h:1-4`), so host code can read a guest address directly once it
is known (`runtime_memory_is_mapped()`, `src/runtime.h:25`; fault-safe reads via
`runtime_fault_recover`).

### 3.2 What the game has — the named variables

`eboot.bin` (CUSA06534 1.14) contains a generated dictionary of ~540 gameplay variables, one
NUL-separated block of `NAME = <id>` strings at file offset ≈ 31,683,000, each name also present
bare. The ids this mod needs:

| key | id |
|---|---|
| `ROUND_NUMBER_CURRENT` | 9910 |
| `ROUND_NUMBER_MAX` | 7241 |
| `ROUND_TIME_ELAPSED` | 3640 |
| `ROUND_TIME_REMAINING` | 4454 |
| `CLOCK_RUNNING` | 4157 |
| `MATCH_STATE` | 6716 |
| `FIGHTER_STRIKES_SIGNIFICANT_LANDED` | 7566 |
| `FIGHTER_STRIKES_SIGNIFICANT_THROWN` | 9567 |
| `FIGHTER_STRIKES_THROWN` | 9391 |
| `FIGHTER_HEADSHOTS_LANDED` | 8850 |
| `FIGHTER_HEADSHOTS_THROWN` | 7077 |
| `FIGHTER_TAKEDOWNS_SUCCESSFUL` | 5685 |
| `FIGHTER_TAKEDOWNS_ATTEMPTED` | 9343 |
| `FIGHTER_KNOCKED_DOWN_COUNT` | 1683 |
| `FIGHTER_STUNNED_COUNT` | 8832 |
| `FIGHTER_HEAD_HEALTH` | 8851 |
| `FIGHTER_OVERALL_HEALTH` | 8927 |
| `FIGHTER_SCORE_CARD` | 7573 |
| `JUDGE1_PICK` / `JUDGE1_SCORE_TOTAL` | 7422 / 4145 |
| `JUDGE2_PICK` / `JUDGE2_SCORE_TOTAL` | 7431 / 4022 |
| `JUDGE3_PICK` / `JUDGE3_SCORE_TOTAL` | 8216 / 2965 |
| `FIGHTER_PUNCH_TYPES_DAMAGE_DETAILED` | 9468 |
| `FIGHTER_KICK_DAMAGE_DETAILED` | 9379 |
| `FIGHTER_KNEE_DAMAGE_DETAILED` | 1684 |
| `FIGHTER_ELBOW_DAMAGE_DETAILED` | 5379 |

Also there: `FIGHTER_TAKEDOWNS_DENIED`, `FIGHTER_STRIKES_BLOCKED`, `FIGHTER_STRIKES_CLINCH_*`,
`FIGHTER_STRIKES_GROUND_*`, `FIGHTER_IS_STUNNED`, `BETWEEN_ROUND_STATE`, `GLOBAL_FIGHT_GAMEMODE`.

Two of the Phase 0 questions are therefore answered by the dictionary alone: round state is a set of
named variables (no need to infer rounds from the screen), and **the game does compute judges'
scores** — `JUDGE1..3_SCORE_TOTAL`, `JUDGE1..3_PICK`, `FIGHTER_SCORE_CARD`, plus the Scaleform
variable names the scorecard movie reads (`@Judge1Score_Red`, `@Judge1Score_Blue`, `@Judge1Pick`,
…). Phase 4 can show the game's own cards; computing our own stays the fallback.

### 3.3 How the values are reached (static analysis of the eboot)

The eboot is a PS4 SELF with no symbol table; a flat mapping VA = file offset − 0x4000 works for the
whole image (checked against `bbport_guest_hooks.cpp` addresses). The tools used are in
`tools/re/` (`strrefs.py`, `funcs.py`, `disas.py`, `ptrrefs.py`, `constrefs.py`).

- **Getter.** `0x1bb180(context, id, out, type)` returns the int value of a variable; it is a thin
  wrapper over a **virtual** member at `[vtable+0x10]`. The setter is `[vtable+0x18]` with
  (context, id, value, type). `0x1bb050()` reads a float by name from a global manager, and
  `0x1bb0a0(context, name, value, out)` writes by name.
- **Schema registry.** The global pointer at `0x2a43820` is a name-hash table of
  `{const char* name; uint32 id; node* next;}` nodes; it is filled by the giant initialiser at
  `0x1d3b60` (each entry: `lea rax, "NAME"; mov ecx, <hash>; call 0x1f2950`; the id sits right
  next to the name pointer).
- **Contexts.** A fighter/match object keeps its stat context at `+0xc8`: `0x1bfcf0` reads
  `ROUND_NUMBER_CURRENT`, `ROUND_TIME_ELAPSED` and a float time from `[obj+0xc8]`. The round-state
  reader is `0x1ba9d0`; it reads ids 9910, 7241, 3640, 4454 (and 2178, 2683) through the getter.
- **No dense id-indexed array.** This was tested at runtime, twice, and is now a negative result:
  892 MiB of mapped guest memory (every region ≥ 64 KiB, image excluded) was scanned for
  (a) a 32-bit or float word that counts down like the round clock over 1.5 s, and (b) a block where
  the `ROUND_*`/`FIGHTER_*` id slots hold the values the pause menu showed at that moment
  (`14/30` significant, `30/62` total, `0/1` takedowns). Nothing plausible came back: the values
  are **per context**, not a global array indexed by id.

So the live values are behind `[vtable+0x10]` on a context object, and the missing link is that
object's address (and, with it, the vtable's implementation of the getter).

### 3.4 Closing it

Two ways, both small:

1. **Capture the context at a call site.** One `int3` at `0x1bb180` would hand us (context, id) for
   every stat read in the game — and the vtable pointer, from which the getter implementation and
   the storage layout can be disassembled. The port's `int3` hook framework is Linux-only
   (`bbport_guest_hooks.cpp`, `#ifdef __linux__`), and the test build here is Windows, so this
   needs a Windows detour (byte patch + trampoline) — new infrastructure, roughly 150 lines, but it
   is what Phases 2-3 will want anyway for per-event momentum.
2. **Find the record from the pause-menu numbers.** The probe now does this: it reads
   `broadcast_values.txt` (`14 30 62 1 10 12 26 35` — the numbers the pause menu shows) and looks
   for a word equal to the first value whose ±2 KiB window holds several of the others. With a
   paused fight that has non-zero stats this locates the record, its layout (which offsets hold
   significant landed/thrown, total, takedowns, knockdowns) and, by extension, the object that owns
   it.

Route 2 is cheaper and needs no guest-code patching. The probe now does the sharp version of it:
`broadcast_values.txt` holds the numbers the pause menu shows, and the scan looks for **(a)** a
*two-value pair* (landed, thrown of the same statistic) at distance 4-16 bytes — two specific 32-bit
values next to each other have almost no false positives in a gigabyte — and **(b)** clusters where
three or more of the values sit within 128 bytes, printed smallest-spread first.

Two runs proved the loose version useless (13264 anchors over 892 MiB, all of them counter arrays of
consecutive integers that merely contain 13, 26, 36, 63), so the pair search is the one to try next.
One practical note for that run: the pause menu can be opened **during the walk-in**, where it shows
`R1 5:00` and all zeros — the numbers only mean something after the bell, i.e. once the clock is
below 5:00. The first attempts were paused there by mistake.

### 3.5 Damage by zone

The dictionary has `FIGHTER_HEAD_HEALTH` and `FIGHTER_OVERALL_HEALTH` and damage **by strike type**
(punch/kick/knee/elbow, `*_DAMAGE_DETAILED`), but no body/leg health or per-zone damage keys. The
pause menu shows only "Body Health Events" and "Leg Health Events" counters (0 in every round seen
so far). Phase 2's "damage by zone" bars therefore have three options: head health + overall health,
the pause menu's event counters, or dropping the zone split in favour of the per-strike-type
breakdown. This is a scope decision, not a coding problem.

## 4. The discovery probe (built, runs on Windows)

`gpu/shim/broadcast/{bbport_probe.h,broadcast_probe.cpp}` — host-only, no guest-code patching, works
on the Windows build the user plays. `BB_BROADCAST_PROBE=1` arms it, **F9** in game starts a scan on
its own thread (a scan freezes nothing; it takes ~2-4 s):

- walks the runtime's mapping table (`runtime_memory_region`), logs every mapped region and its size;
- reads every region fault-safely, so a scan cannot crash the game;
- reports, to stdout and to `broadcast_probe.log` next to `bbport.ini`:
  - the region census,
  - `broadcast_clock.txt` values (word search for the HUD clock),
  - `broadcast_values.txt` anchors (the pause-menu statistics),
  - a countdown diff (two passes 1.5 s apart) as a fallback.

Build/stage used for the test: `BB_LTO=OFF bash scripts/build-windows.sh --runtime`, then a copy of
the package with `bin/sacredfire-game.exe` replaced; that folder is
`D:\Games\UFC3Native\UFC3Native-probe` and its launcher sets `BB_BROADCAST_PROBE=1`.

## 5. Phase 1 plan (no game data needed)

New code in `gpu/shim/broadcast/` (picked up by the shim glob `shim/*/*.cpp`,
`gpu/CMakeLists.txt:158`), ~250 lines:

- `broadcast_config.{h,cpp}` — `broadcast_mod.ini`: `enabled`, `stat_popups`, `momentum_meter`,
  `scorecard_reveal`, `ui_scale`, momentum point values, timings; missing file → defaults;
  `enabled = false` → module inert (no ImGui call, no allocation).
- `broadcast_overlay.{h,cpp}` — `Visible()`, `Draw()`, `Tick()`: text, filled rect, bar, fade, all in
  viewport-relative coordinates multiplied by `ui_scale`.
- hooks: `Visible()` in `bbport_overlay.cpp:1107`, one `Broadcast::Draw()` in `Render()` after
  `ImGui::NewFrame()`, one key case in `HandleEvent()`.
- Phase 1 content: "Broadcast mod loaded" bottom-centre + F8 toggle — exactly the phase's done
  criterion.

## 6. Open questions

1. May the new folder be `gpu/shim/broadcast/` rather than `src/mods/broadcast/`? The overlay is a
   C++ host-side module linked into `libbbgpu`; `src/` is the C runtime and cannot call ImGui.
2. Route 1 (Windows detour) or route 2 (pause-menu anchors) for the data? Route 2 is cheaper today;
   route 1 is the foundation for per-event momentum.
