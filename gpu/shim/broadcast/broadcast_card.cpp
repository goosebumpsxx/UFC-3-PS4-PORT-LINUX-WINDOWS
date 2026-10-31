// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 2. The broadcast-style stat card.
//
// Bottom-centre, both fighters side by side, home (left/red) and away (right/blue) the way the
// game pause-menu statistics page lays them out. Rows: significant strikes landed/thrown and the
// percentage, total strikes landed, takedowns landed/attempted, knockdowns, then the two damage
// bars (head health and overall health, the Phase 2 default).
//
// Three ways it appears:
//   * popup_delay seconds into rounds 2 or later: the round before, read straight out of that
//     round cells;
//   * when a fighter FIGHTER_KNOCKED_DOWN_COUNT rises: a short KNOCKDOWN card;
//   * on the hotkey (card_key, F7): the fight so far, from the game own fight total.
#include "broadcast_card.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

#include <SDL3/SDL_keycode.h>
#include "broadcast_config.h"
#include "broadcast_overlay.h"
#include "broadcast_stats.h"

namespace Broadcast {
namespace Card {
namespace {

using Stats::Knockdown;
using Stats::KnockdownKind;
using Stats::Snapshot;

constexpr std::uint32_t PanelColour = Rgba(6, 8, 12, 196);
constexpr std::uint32_t HeaderHome = Rgba(255, 88, 78, 255);
constexpr std::uint32_t HeaderAway = Rgba(86, 156, 255, 255);
constexpr std::uint32_t LabelColour = Rgba(176, 186, 198, 255);
constexpr std::uint32_t ValueColour = Rgba(240, 244, 248, 255);
constexpr std::uint32_t DimColour = Rgba(140, 150, 162, 255);
constexpr std::uint32_t HomeBar = Rgba(232, 74, 64, 235);
constexpr std::uint32_t AwayBar = Rgba(64, 136, 240, 235);
constexpr std::uint32_t TrackColour = Rgba(255, 255, 255, 40);

/// Fade at both ends, in seconds.
constexpr float FadeSeconds = 0.5f;

enum class Mode {
    None,
    PreviousRound,
    Knockdown,
    FightSoFar,
};

struct State {
    Mode mode = Mode::None;
    double shownAt = 0.0;
    float duration = 0.0f;
    Snapshot snapshot;
    int round = 0; ///< the round the card describes; 0 = the fight so far
    KnockdownKind victim = KnockdownKind::None;
    int knockdownCount = 0;
    double roundStartedAt = 0.0;
    int roundStarted = 0;
    bool previousRoundShown = false;
    std::uint64_t knockdownSerial = 0;
};

State state;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

/// "13 / 23 (57%)"; a zero denominator is a plain 0 and never a division.
void StrikeText(char* out, std::size_t size, int landed, int thrown) {
    if (thrown <= 0) {
        std::snprintf(out, size, "%d / %d", landed, std::max(thrown, 0));
        return;
    }
    const int percent = int(((landed * 100) + (thrown / 2)) / thrown);
    std::snprintf(out, size, "%d / %d (%d%%)", landed, thrown, percent);
}

/// The knockdown row shows the opponent count, which is what the game own stats page shows.
Snapshot Display(const Snapshot& raw) {
    Snapshot shown = raw;
    shown.home.knockedDown = raw.away.knockedDown;
    shown.away.knockedDown = raw.home.knockedDown;
    return shown;
}

void Ordinal(int count, char* out, std::size_t size) {
    const char* suffix = "th";
    if (count % 100 < 11 || count % 100 > 13) {
        switch (count % 10) {
        case 1: suffix = "st"; break;
        case 2: suffix = "nd"; break;
        case 3: suffix = "rd"; break;
        default: break;
        }
    }
    std::snprintf(out, size, "%d%s", count, suffix);
}

float AlphaOf(double now) {
    const float elapsed = float(now - state.shownAt);
    const float fade = std::min(FadeSeconds, state.duration * 0.25f);
    if (elapsed < fade) {
        return std::max(0.0f, elapsed / fade);
    }
    const float left = state.duration - elapsed;
    if (left < fade) {
        return std::max(0.0f, left / fade);
    }
    return 1.0f;
}

std::uint32_t Fade(std::uint32_t colour, float alpha) {
    const std::uint32_t a = (colour >> 24) & 0xFFu;
    const std::uint32_t faded = std::uint32_t(float(a) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & 0x00FFFFFFu) | (faded << 24);
}

void Show(Mode mode, float duration) {
    state.mode = mode;
    state.shownAt = Now();
    state.duration = duration;
}

/// The card frame: bottom-centre, above the game clock strip and clear of the health bars.
struct Layout {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float line = 0.0f;
    float pad = 0.0f;
    float barHeight = 0.0f;
};

Layout ComputeLayout(const Canvas& canvas, int valueRows) {
    Layout layout;
    layout.line = TextLineHeight(canvas) * 1.62f;
    layout.pad = 14.0f * canvas.scale;
    layout.barHeight = 8.0f * canvas.scale;
    const float rows = layout.line * float(valueRows + 1) + layout.barHeight * 2.0f +
                       layout.line * 0.45f * 2.0f;
    layout.width = 0.62f * canvas.width;
    layout.height = layout.pad * 2.0f + rows;
    layout.left = 0.5f * (canvas.width - layout.width);
    // Above the clock and round strip (which sit at 87-91 % of the height), below the fighters.
    layout.top = 0.83f * canvas.height - layout.height;
    return layout;
}

void DrawFrame(const Canvas& canvas, const Layout& layout, float alpha) {
    DrawRect(canvas, layout.left / canvas.width, layout.top / canvas.height,
             layout.width / canvas.width, layout.height / canvas.height, Fade(PanelColour, alpha),
             10.0f * canvas.scale);
    // A hairline along the top edge, in the corner colours: red left, blue right.
    DrawRect(canvas, layout.left / canvas.width, layout.top / canvas.height,
             layout.width / canvas.width * 0.5f, 2.0f / canvas.height, Fade(HomeBar, alpha));
    DrawRect(canvas, (layout.left + layout.width * 0.5f) / canvas.width, layout.top / canvas.height,
             layout.width / canvas.width * 0.5f, 2.0f / canvas.height, Fade(AwayBar, alpha));
}

/// The heading row: RED on the left, the title in the middle, BLUE on the right.
void DrawHeading(const Canvas& canvas, const Layout& layout, const char* title, float alpha) {
    const float y = (layout.top + layout.pad) / canvas.height;
    DrawText(canvas, (layout.left + layout.pad) / canvas.width, y, "RED", Fade(HeaderHome, alpha),
             TextAlign::Left);
    DrawText(canvas, 0.5f, y, title, Fade(ValueColour, alpha), TextAlign::Centre);
    DrawText(canvas, (layout.left + layout.width - layout.pad) / canvas.width, y, "BLUE",
             Fade(HeaderAway, alpha), TextAlign::Right);
}

/// The stat rows, shared by the previous-round and the on-demand card.
void DrawStats(const Canvas& canvas, Layout layout, const Snapshot& raw, float alpha) {
    const Snapshot shown = Display(raw);
    layout.top += layout.line * 1.30f;
    const float homeX = (layout.left + layout.pad) / canvas.width;
    const float awayX = (layout.left + layout.width - layout.pad) / canvas.width;
    float y = (layout.top + layout.pad) / canvas.height;

    char home[64];
    char away[64];

    StrikeText(home, sizeof(home), shown.home.sigLanded, shown.home.sigThrown);
    StrikeText(away, sizeof(away), shown.away.sigLanded, shown.away.sigThrown);
    DrawText(canvas, homeX, y, home, Fade(ValueColour, alpha), TextAlign::Left);
    DrawText(canvas, 0.5f, y, "Significant strikes", Fade(LabelColour, alpha), TextAlign::Centre);
    DrawText(canvas, awayX, y, away, Fade(ValueColour, alpha), TextAlign::Right);
    y += layout.line / canvas.height;

    std::snprintf(home, sizeof(home), "%d", shown.home.totalLanded);
    std::snprintf(away, sizeof(away), "%d", shown.away.totalLanded);
    DrawText(canvas, homeX, y, home, Fade(ValueColour, alpha), TextAlign::Left);
    DrawText(canvas, 0.5f, y, "Total strikes landed", Fade(LabelColour, alpha), TextAlign::Centre);
    DrawText(canvas, awayX, y, away, Fade(ValueColour, alpha), TextAlign::Right);
    y += layout.line / canvas.height;

    std::snprintf(home, sizeof(home), "%d / %d", shown.home.takedownsLanded,
                  std::max(shown.home.takedownsAttempted, 0));
    std::snprintf(away, sizeof(away), "%d / %d", shown.away.takedownsLanded,
                  std::max(shown.away.takedownsAttempted, 0));
    DrawText(canvas, homeX, y, home, Fade(ValueColour, alpha), TextAlign::Left);
    DrawText(canvas, 0.5f, y, "Takedowns", Fade(LabelColour, alpha), TextAlign::Centre);
    DrawText(canvas, awayX, y, away, Fade(ValueColour, alpha), TextAlign::Right);
    y += layout.line / canvas.height;

    std::snprintf(home, sizeof(home), "%d", shown.home.knockedDown);
    std::snprintf(away, sizeof(away), "%d", shown.away.knockedDown);
    DrawText(canvas, homeX, y, home, Fade(ValueColour, alpha), TextAlign::Left);
    DrawText(canvas, 0.5f, y, "Knockdowns", Fade(LabelColour, alpha), TextAlign::Centre);
    DrawText(canvas, awayX, y, away, Fade(ValueColour, alpha), TextAlign::Right);
    y += (layout.line + layout.line * 0.45f) / canvas.height;

    // The damage bars: head health, then overall health (the Phase 2 default).
    const float barWidth = layout.width * 0.34f / canvas.width;
    const float barHeight = layout.barHeight / canvas.height;
    const float awayBarX = awayX - barWidth;
    for (int row = 0; row < 2; ++row) {
        const bool head = row == 0;
        const char* name = head ? "Head" : "Overall";
        const int homeValue = head ? shown.home.headHealth : shown.home.overallHealth;
        const int awayValue = head ? shown.away.headHealth : shown.away.overallHealth;
        DrawBar(canvas, homeX, y, barWidth, barHeight, float(homeValue) / 100.0f,
                Fade(HomeBar, alpha), Fade(TrackColour, alpha));
        DrawBar(canvas, awayBarX, y, barWidth, barHeight, float(awayValue) / 100.0f,
                Fade(AwayBar, alpha), Fade(TrackColour, alpha));
        DrawText(canvas, 0.5f, y - ((layout.line - layout.barHeight) * 0.5f) / canvas.height, name,
                 Fade(DimColour, alpha), TextAlign::Centre);
        y += (layout.barHeight + layout.line * 0.45f) / canvas.height;
    }
}

void DrawPreviousRound(const Canvas& canvas, float alpha) {
    const Layout layout = ComputeLayout(canvas, 4);
    DrawFrame(canvas, layout, alpha);
    char header[32];
    std::snprintf(header, sizeof(header), "ROUND %d", state.round);
    DrawHeading(canvas, layout, header, alpha);
    DrawStats(canvas, layout, state.snapshot, alpha);
}

void DrawFightSoFar(const Canvas& canvas, float alpha) {
    const Layout layout = ComputeLayout(canvas, 4);
    DrawFrame(canvas, layout, alpha);
    DrawHeading(canvas, layout, state.round >= 2 ? "FIGHT SO FAR" : "ROUND 1 SO FAR", alpha);
    DrawStats(canvas, layout, state.snapshot, alpha);
}

void DrawKnockdown(const Canvas& canvas, float alpha) {
    Layout layout = ComputeLayout(canvas, 0);
    layout.height = layout.pad * 2.0f + layout.line * 1.9f;
    layout.top = 0.83f * canvas.height - layout.height;
    DrawFrame(canvas, layout, alpha);

    const bool homeDown = state.victim == KnockdownKind::HomeDropped;
    const char* victim = homeDown ? "RED" : "BLUE";
    const char* scorer = homeDown ? "BLUE" : "RED";
    const std::uint32_t victimColour = homeDown ? HeaderHome : HeaderAway;

    char head[96];
    std::snprintf(head, sizeof(head), "KNOCKDOWN - %s drops %s", scorer, victim);
    DrawText(canvas, 0.5f, (layout.top + layout.pad) / canvas.height, head,
             Fade(victimColour, alpha), TextAlign::Centre);

    char count[64];
    char ordinal[16];
    Ordinal(state.knockdownCount, ordinal, sizeof(ordinal));
    std::snprintf(count, sizeof(count), "%s of the fight", ordinal);
    DrawText(canvas, 0.5f, (layout.top + layout.pad + layout.line * 1.05f) / canvas.height, count,
             Fade(LabelColour, alpha), TextAlign::Centre);
}

} // namespace

void Init() {
    if (CardKeyCode() == std::uint32_t(SDLK_UNKNOWN)) {
        std::printf("Broadcast: card_key %s is not F1-F12 or A-Z, using F7\n",
                    GetConfig().card_key.c_str());
    }
}

void Tick(const Canvas& canvas) {
    const Config& config = GetConfig();
    if (!config.enabled || !config.stat_popups) {
        state.mode = Mode::None;
        return;
    }
    const double now = Now();

    if (!Stats::InFight()) {
        state.mode = Mode::None;
        state.roundStarted = 0;
        state.previousRoundShown = false;
        return;
    }

    // A round change starts the clock for the previous-round card.
    const int round = Stats::Round();
    if (round != state.roundStarted) {
        state.roundStarted = round;
        state.roundStartedAt = now;
        state.previousRoundShown = false;
    }

    // A knockdown interrupts whatever is up: it is the moment people watch for.
    const Knockdown knockdown = Stats::PollKnockdown();
    if (knockdown.kind != KnockdownKind::None && knockdown.serial != 0 &&
        knockdown.serial != state.knockdownSerial) {
        state.knockdownSerial = knockdown.serial;
        state.victim = knockdown.kind;
        state.knockdownCount = knockdown.count;
        Show(Mode::Knockdown, config.knockdown_seconds);
    }

    // The scheduled previous-round card: once per round, and only while the fight is running.
    if (!state.previousRoundShown && round >= 2 && !Stats::Paused() &&
        now - state.roundStartedAt >= double(config.popup_delay)) {
        Snapshot previous;
        if (Stats::PreviousRound(previous)) {
            state.snapshot = previous;
            state.round = round - 1;
            state.previousRoundShown = true;
            Show(Mode::PreviousRound, config.popup_seconds);
            Printf("Broadcast: card: round %d stats, %.1f s into round %d\n", state.round,
                   now - state.roundStartedAt, round);
        }
    }

    if (state.mode == Mode::None || now - state.shownAt > double(state.duration)) {
        state.mode = Mode::None;
        return;
    }
    // Never over the pause menu, a menu or the between-rounds screen.
    if (Stats::Paused()) {
        return;
    }

    const float alpha = AlphaOf(now);
    switch (state.mode) {
    case Mode::PreviousRound:
        DrawPreviousRound(canvas, alpha);
        break;
    case Mode::FightSoFar:
        DrawFightSoFar(canvas, alpha);
        break;
    case Mode::Knockdown:
        DrawKnockdown(canvas, alpha);
        break;
    case Mode::None:
        break;
    }
}

bool HandleKey(std::uint32_t key) {
    const std::uint32_t code = CardKeyCode();
    if (code == std::uint32_t(SDLK_UNKNOWN) || key != code) {
        return false;
    }
    const Config& config = GetConfig();
    if (!config.enabled || !config.stat_popups || !Stats::InFight()) {
        return false;
    }
    Snapshot total;
    if (!Stats::FightSoFar(total)) {
        return false;
    }
    if (config.dump_stats) {
        Stats::Dump();
    }
    state.snapshot = total;
    state.round = Stats::Round();
    Show(Mode::FightSoFar, config.popup_seconds);
    Printf("Broadcast: card: fight so far, on demand (round %d)\n", state.round);
    return true;
}

} // namespace Card
} // namespace Broadcast
