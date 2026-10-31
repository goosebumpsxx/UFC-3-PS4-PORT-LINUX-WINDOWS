// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 1. broadcast_mod.ini lives next to bbport.ini, so the mod's
// knobs stay out of the port's own settings file. Read once at start; a broken file never stops the
// game, it just keeps the defaults and says so in the log.
#include "broadcast_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL_keycode.h>

namespace Broadcast {

namespace {

Config config;

/// broadcast_mod.ini in the folder of BB_CONFIG (the launcher passes an absolute bbport.ini), else
/// in the current folder — the same rule broadcast_probe.cpp uses for its text files.
std::string ConfigPath() {
    const char* env = std::getenv("BB_CONFIG");
    std::string path;
    if (env && env[0]) {
        path = env;
        const std::size_t slash = path.find_last_of("/\\");
        path.resize(slash == std::string::npos ? 0 : slash + 1);
    }
    path += "broadcast_mod.ini";
    return path;
}

std::string Trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

std::string Lower(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return lower;
}

std::string Upper(const std::string& text) {
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return upper;
}

bool ParseBool(const std::string& value, bool fallback, const char* key) {
    const std::string lower = Lower(value);
    if (lower == "true" || lower == "1" || lower == "on" || lower == "yes") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "off" || lower == "no") {
        return false;
    }
    std::printf("Broadcast: %s = '%s' is not true/false, keeping %s\n", key, value.c_str(),
                fallback ? "true" : "false");
    return fallback;
}

/// F1-F12 and A-Z; anything else is rejected so a typo cannot silently bind a key nobody expects.
std::uint32_t KeyFromName(const std::string& name) {
    const std::string upper = Upper(Trim(name));
    if (upper.size() == 1 && upper[0] >= 'A' && upper[0] <= 'Z') {
        return std::uint32_t(SDLK_A) + std::uint32_t(upper[0] - 'A');
    }
    if (upper.size() >= 2 && upper.size() <= 3 && upper[0] == 'F') {
        const int number = std::atoi(upper.c_str() + 1);
        if (number >= 1 && number <= 12) {
            return std::uint32_t(SDLK_F1) + std::uint32_t(number - 1);
        }
    }
    return std::uint32_t(SDLK_UNKNOWN);
}

/// A seconds value: clamped to [low, high]; bad text keeps the default and says so once.
float ParseSeconds(const std::string& value, float fallback, const char* key, float low,
                   float high) {
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !(parsed >= 0.0f)) {
        std::printf("Broadcast: %s = '%s' is not a number, keeping %.2f\n", key, value.c_str(),
                    double(fallback));
        return fallback;
    }
    return std::clamp(parsed, low, high);
}

void WriteDefaults(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) {
        std::printf("Broadcast: cannot write %s, using the defaults\n", path.c_str());
        return;
    }
    std::fprintf(file,
                 "# Broadcast mod settings.\n"
                 "# enabled: master switch. stat_popups: the Phase 2 card. ui_scale: 0.5-3.0.\n"
                 "# popup_seconds / popup_delay / knockdown_seconds: the card's timing, in seconds.\n"
                 "# card_key: show the fight so far on demand. debug_card: the Phase 1 demo card.\n"
                 "# dump_stats: log the raw stat cells when the card key is pressed (a support aid).\n"
                 "# momentum_*: the Phase 3 meter: points per event and how fast it drifts back to 0.\n"
                 "enabled = true\n"
                 "stat_popups = true\n"
                 "momentum_meter = true\n"
                 "scorecard_reveal = true\n"
                 "ui_scale = 1.0\n"
                 "toggle_key = F8\n"
                 "popup_seconds = 5\n"
                 "popup_delay = 15\n"
                 "knockdown_seconds = 3\n"
                 "card_key = F7\n"
                 "debug_card = false\n"
                 "dump_stats = false\n"
                 "momentum_sig_strike = 3\n"
                 "momentum_head_strike = 4\n"
                 "momentum_takedown = 8\n"
                 "momentum_knockdown = 25\n"
                 "momentum_near_finish = 10\n"
                 "momentum_decay = 2\n");
    std::fclose(file);
    std::printf("Broadcast: wrote the defaults to %s\n", path.c_str());
}

} // namespace

void LoadConfig() {
    const std::string path = ConfigPath();
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        WriteDefaults(path);
        std::printf("Broadcast: enabled=%d ui_scale=%.2f toggle_key=%s\n", config.enabled ? 1 : 0,
                    double(config.ui_scale), config.toggle_key.c_str());
        return;
    }

    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const std::string text = Trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') {
            continue;
        }
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) {
            std::printf("Broadcast: ignoring '%s' (no '=')\n", text.c_str());
            continue;
        }
        const std::string key = Lower(Trim(text.substr(0, equals)));
        const std::string value = Trim(text.substr(equals + 1));

        if (key == "enabled") {
            config.enabled = ParseBool(value, config.enabled, "enabled");
        } else if (key == "stat_popups") {
            config.stat_popups = ParseBool(value, config.stat_popups, "stat_popups");
        } else if (key == "momentum_meter") {
            config.momentum_meter = ParseBool(value, config.momentum_meter, "momentum_meter");
        } else if (key == "scorecard_reveal") {
            config.scorecard_reveal = ParseBool(value, config.scorecard_reveal, "scorecard_reveal");
        } else if (key == "ui_scale") {
            char* end = nullptr;
            const float parsed = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !(parsed > 0.0f)) {
                std::printf("Broadcast: ui_scale = '%s' is not a number, keeping %.2f\n",
                            value.c_str(), double(config.ui_scale));
            } else {
                config.ui_scale = std::clamp(parsed, 0.5f, 3.0f);
            }
        } else if (key == "toggle_key") {
            if (KeyFromName(value) == std::uint32_t(SDLK_UNKNOWN)) {
                std::printf("Broadcast: toggle_key = '%s' is not F1-F12 or A-Z, keeping %s\n",
                            value.c_str(), config.toggle_key.c_str());
            } else {
                config.toggle_key = Upper(value);
            }
        } else if (key == "popup_seconds") {
            config.popup_seconds = ParseSeconds(value, config.popup_seconds, "popup_seconds", 1.0f, 30.0f);
        } else if (key == "popup_delay") {
            config.popup_delay = ParseSeconds(value, config.popup_delay, "popup_delay", 0.0f, 300.0f);
        } else if (key == "knockdown_seconds") {
            config.knockdown_seconds =
                ParseSeconds(value, config.knockdown_seconds, "knockdown_seconds", 1.0f, 30.0f);
        } else if (key == "card_key") {
            if (KeyFromName(value) == std::uint32_t(SDLK_UNKNOWN)) {
                std::printf("Broadcast: card_key = '%s' is not F1-F12 or A-Z, keeping %s\n",
                            value.c_str(), config.card_key.c_str());
            } else {
                config.card_key = Upper(value);
            }
        } else if (key == "debug_card") {
            config.debug_card = ParseBool(value, config.debug_card, "debug_card");
        } else if (key == "dump_stats") {
            config.dump_stats = ParseBool(value, config.dump_stats, "dump_stats");
        } else if (key == "momentum_sig_strike") {
            config.momentum_sig_strike = ParseSeconds(value, config.momentum_sig_strike,
                                                      "momentum_sig_strike", 0.0f, 100.0f);
        } else if (key == "momentum_head_strike") {
            config.momentum_head_strike = ParseSeconds(value, config.momentum_head_strike,
                                                       "momentum_head_strike", 0.0f, 100.0f);
        } else if (key == "momentum_takedown") {
            config.momentum_takedown = ParseSeconds(value, config.momentum_takedown,
                                                    "momentum_takedown", 0.0f, 100.0f);
        } else if (key == "momentum_knockdown") {
            config.momentum_knockdown = ParseSeconds(value, config.momentum_knockdown,
                                                     "momentum_knockdown", 0.0f, 100.0f);
        } else if (key == "momentum_near_finish") {
            config.momentum_near_finish = ParseSeconds(value, config.momentum_near_finish,
                                                       "momentum_near_finish", 0.0f, 100.0f);
        } else if (key == "momentum_decay") {
            config.momentum_decay = ParseSeconds(value, config.momentum_decay, "momentum_decay",
                                                 0.0f, 50.0f);
        }
        // Unknown keys are ignored: later phases will add their own.
    }
    std::fclose(file);
    std::printf("Broadcast: %s: enabled=%d ui_scale=%.2f toggle_key=%s\n", path.c_str(),
                config.enabled ? 1 : 0, double(config.ui_scale), config.toggle_key.c_str());
}

const Config& GetConfig() {
    return config;
}

std::uint32_t ToggleKeyCode() {
    return KeyFromName(config.toggle_key);
}

std::uint32_t CardKeyCode() {
    return KeyFromName(config.card_key);
}

} // namespace Broadcast
