// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast mod discovery probe (BB_BROADCAST_PROBE=1, F9 in game).
//
// Phase 0 of the broadcast overlay mod needs to know where the game keeps its per-fight
// variables. The game reads them through a getter by stat id (guest 0x1bb180, ids from the
// dictionary in eboot.bin), but the container behind that getter is not known yet.
//
// This probe tests the cheapest hypothesis without touching game code: that the values live in
// a block indexed by stat id, e.g. round_number at +9910*4 and round_time_remaining at +4454*4.
// It finds the clock without knowing the base by diffing guest memory over 1.5 s: a slot that
// went 2xx -> 2xx-2 while staying in 0..600 is a countdown. From that address every stride
// (4, 8, 16 bytes per id) gives a candidate base, and a base whose +9910 slot holds 1..5 is
// printed with all the slots the mod needs.
//
// Everything is read-only and fault safe (the runtime's speculative-read recovery), so a scan
// can neither crash the game nor change its state. The result goes to stdout and to
// broadcast_probe.log next to bbport.ini.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "bbport_probe.h"
#include "bbport_toggles.h" // runtime_fault_recover, bb_fault_jmp_buf

extern "C" int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                     int* mapped);

namespace BroadcastProbe {
namespace {

constexpr uintptr_t ImageBase = 0x800000000ull;
constexpr uint64_t ImageSize = 0x2b20000ull; // eboot image, never scanned

// The stat ids of the dictionary in eboot.bin (name = id).
enum : int {
    IdSignificantLanded = 7566,
    IdSignificantThrown = 9567,
    IdStrikesThrown = 9391,
    IdHeadshotsLanded = 8850,
    IdHeadshotsThrown = 7077,
    IdTakedownsLanded = 5685,
    IdTakedownsAttempted = 9343,
    IdKnockedDown = 1683,
    IdStunned = 8832,
    IdHeadHealth = 8851,
    IdOverallHealth = 8927,
    IdRoundNumber = 9910,
    IdRoundMax = 7241,
    IdRoundElapsed = 3640,
    IdRoundRemaining = 4454,
    IdClockRunning = 4157,
    IdScoreCard = 7573,
    IdJudge1Total = 4145,
    IdJudge2Total = 4022,
    IdJudge3Total = 2965,
};

struct Slot {
    const char* name;
    int id;
};

constexpr Slot Slots[] = {
    {"round_number", IdRoundNumber},
    {"round_max", IdRoundMax},
    {"round_time_elapsed", IdRoundElapsed},
    {"round_time_remaining", IdRoundRemaining},
    {"clock_running", IdClockRunning},
    {"sig_strikes_landed", IdSignificantLanded},
    {"sig_strikes_thrown", IdSignificantThrown},
    {"strikes_thrown", IdStrikesThrown},
    {"headshots_landed", IdHeadshotsLanded},
    {"headshots_thrown", IdHeadshotsThrown},
    {"takedowns_landed", IdTakedownsLanded},
    {"takedowns_attempted", IdTakedownsAttempted},
    {"knocked_down", IdKnockedDown},
    {"stunned", IdStunned},
    {"head_health", IdHeadHealth},
    {"overall_health", IdOverallHealth},
    {"score_card", IdScoreCard},
    {"judge1_total", IdJudge1Total},
    {"judge2_total", IdJudge2Total},
    {"judge3_total", IdJudge3Total},
};

/// All output, to stdout (the game's log) and to broadcast_probe.log next to bbport.ini.
struct Log {
    FILE* file = nullptr;
    Log() {
        char path[1024] = "broadcast_probe.log";
        if (const char* config = std::getenv("BB_CONFIG"); config && *config) {
            std::snprintf(path, sizeof(path), "%s", config);
            if (char* slash = std::strrchr(path, '/')) {
                slash[1] = '\0';
                std::snprintf(path + std::strlen(path), sizeof(path) - std::strlen(path),
                              "broadcast_probe.log");
            } else {
                std::snprintf(path, sizeof(path), "broadcast_probe.log");
            }
        }
        file = std::fopen(path, "w");
    }
    ~Log() {
        if (file) {
            std::fclose(file);
        }
    }
    void operator()(const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vprintf(format, args);
        va_end(args);
        if (file) {
            va_start(args, format);
            std::vfprintf(file, format, args);
            va_end(args);
            std::fflush(file);
        }
    }
};

/// A read that a fault cannot kill: the platform's guest-read recovery resumes it here.
bool SafeCopy(void* destination, uintptr_t source, std::size_t size) {
    bb_fault_jmp_buf buffer;
    bb_fault_jmp_buf* previous = runtime_fault_recover;
    if (bb_fault_setjmp(buffer) != 0) {
        runtime_fault_recover = previous;
        return false;
    }
    runtime_fault_recover = &buffer;
    std::memcpy(destination, reinterpret_cast<const void*>(source), size);
    runtime_fault_recover = previous;
    return true;
}

bool ReadWord(uintptr_t address, std::uint32_t& value) {
    return SafeCopy(&value, address, sizeof(value));
}

struct Region {
    uintptr_t start, end;
    bool operator<(const Region& other) const { return (end - start) > (other.end - other.start); }
};

std::vector<Region> MappedRegions(Log& log) {
    std::vector<Region> regions;
    uintptr_t at = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(at, &start, &end, &mapped)) {
            break;
        }
        if (end <= at) {
            break;
        }
        if (mapped && end > start) {
            regions.push_back({start, end});
        }
        at = end;
    }
    log("Probe: %zu mapped regions\n", regions.size());
    std::sort(regions.begin(), regions.end());
    return regions;
}

/// Guest regions worth scanning: not the image, not enormous GPU allocations.
std::vector<Region> ScanSet(const std::vector<Region>& regions, std::uint64_t budget,
                            std::uint64_t per_region, Log& log) {
    std::vector<Region> set;
    std::uint64_t used = 0;
    for (const Region& region : regions) {
        const std::uint64_t size = region.end - region.start;
        log("Probe: region 0x%llx..0x%llx  %.1f MiB\n", (unsigned long long)region.start,
            (unsigned long long)region.end, double(size) / (1 << 20));
        if (size < 0x10000 || size > (4ull << 30)) {
            continue;
        }
        if (region.start < ImageBase + ImageSize && region.end > ImageBase) {
            continue; // the game's own image
        }
        if (used >= budget) {
            continue;
        }
        const std::uint64_t take = std::min<std::uint64_t>(size, per_region);
        set.push_back({region.start, region.start + take});
        used += take;
    }
    log("Probe: scanning %zu regions, %.1f MiB\n", set.size(), double(used) / (1 << 20));
    return set;
}

void PrintSlots(Log& log, uintptr_t base, int stride) {
    log("Probe:   base 0x%llx stride %d:\n", (unsigned long long)base, stride);
    for (const Slot& slot : Slots) {
        std::uint32_t raw = 0;
        const uintptr_t at = base + uintptr_t(slot.id) * uintptr_t(stride);
        if (!ReadWord(at, raw)) {
            log("Probe:     %-22s id %5d  <unmapped>\n", slot.name, slot.id);
            continue;
        }
        float as_float = 0.0f;
        std::memcpy(&as_float, &raw, sizeof(as_float));
        log("Probe:     %-22s id %5d  int %-8d 0x%08x  float %.3f\n", slot.name, slot.id,
            int(std::int32_t(raw)), raw, double(as_float));
    }
}

void Dump(Log& log, uintptr_t address, std::size_t bytes) {
    unsigned char buffer[256];
    bytes = std::min<std::size_t>(bytes, sizeof(buffer));
    if (!SafeCopy(buffer, address, bytes)) {
        log("Probe:   <unmapped at 0x%llx>\n", (unsigned long long)address);
        return;
    }
    for (std::size_t i = 0; i < bytes; i += 16) {
        char line[128];
        int n = 0;
        for (std::size_t j = 0; j < 16 && i + j < bytes; ++j) {
            n += std::snprintf(line + n, sizeof(line) - n, "%02x ", buffer[i + j]);
        }
        log("Probe:   +%03zx  %s\n", i, line);
    }
}

/// True when the block at `base` looks like the stat block: a round number 1..5, a plausible
/// round length, and an elapsed time that fits it.
bool LooksLikeStatBlock(uintptr_t base, int stride) {
    std::uint32_t round = 0, remaining = 0, max = 0, elapsed = 0;
    if (!ReadWord(base + uintptr_t(IdRoundNumber) * stride, round) ||
        !ReadWord(base + uintptr_t(IdRoundRemaining) * stride, remaining) ||
        !ReadWord(base + uintptr_t(IdRoundMax) * stride, max) ||
        !ReadWord(base + uintptr_t(IdRoundElapsed) * stride, elapsed)) {
        return false;
    }
    const std::int32_t rounds = std::int32_t(round);
    const std::int32_t clock = std::int32_t(remaining);
    const std::int32_t total = std::int32_t(max);
    const std::int32_t spent = std::int32_t(elapsed);
    return rounds >= 1 && rounds <= 5 && total >= 2 && total <= 6 && clock >= 0 && clock <= 900 &&
           spent >= 0 && spent <= 3600;
}

/// Numbers the tester read off the pause menu's live statistics, from broadcast_values.txt next to
/// bbport.ini (e.g. "14 30 62 1 10 12 26 35": significant landed/thrown, total thrown, takedowns,
/// and the same for the other corner). Finding several of them close together locates the fight's
/// statistics record without knowing its address.
int TargetValues(std::uint32_t* values, int max) {
    char path[1024] = "broadcast_values.txt";
    if (const char* config = std::getenv("BB_CONFIG"); config && *config) {
        std::snprintf(path, sizeof(path), "%s", config);
        if (char* slash = std::strrchr(path, '/')) {
            slash[1] = '\0';
            std::snprintf(path + std::strlen(path), sizeof(path) - std::strlen(path),
                          "broadcast_values.txt");
        } else {
            std::snprintf(path, sizeof(path), "broadcast_values.txt");
        }
    }
    FILE* file = std::fopen(path, "r");
    if (!file) {
        return 0;
    }
    int count = 0;
    int value = 0;
    while (count < max && std::fscanf(file, "%d", &value) == 1) {
        if (value >= 0 && value <= 100000) {
            values[count++] = std::uint32_t(value);
        }
    }
    std::fclose(file);
    return count;
}

/// Find the statistics record by its numbers: every word equal to one of them is a hit, and a
/// cluster of hits (several distinct values within 128 bytes, smallest spread first) is the record.
/// Ints and floats are both tried; the values come from the pause menu, so several share one object.
int ScanForValues(const std::uint32_t* values, int value_count, const std::vector<Region>& regions,
                  Log& log) {
    struct Hit {
        uintptr_t address;
        int index;
    };
    std::vector<Hit> hits;
    for (const Region& region : regions) {
        const std::size_t count = std::size_t((region.end - region.start) / 4);
        const std::size_t chunk = 16384;
        for (std::size_t at = 0; at < count; at += chunk) {
            const std::size_t n = std::min(chunk, count - at);
            std::uint32_t buffer[16384];
            if (!SafeCopy(buffer, region.start + at * 4, n * 4)) {
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t word = buffer[i];
                if (word > 100000) {
                    continue; // all anchors are small
                }
                for (int v = 0; v < value_count; ++v) {
                    if (word == values[v]) {
                        hits.push_back({region.start + (at + i) * 4, v});
                        break;
                    }
                }
                if (hits.size() > 2000000) {
                    break;
                }
            }
        }
    }
    log("Probe: %zu value hit(s)\n", hits.size());
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.address < b.address; });

    // Cluster: for every hit, the hits within 128 bytes; keep the clusters with the most distinct
    // values and the smallest spread.
    struct Cluster {
        uintptr_t first, last;
        int distinct;
        std::vector<Hit> members;
    };
    std::vector<Cluster> clusters;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        while (hits[i].address - hits[begin].address > 128) {
            ++begin;
        }
        std::vector<Hit> members(hits.begin() + long(begin), hits.begin() + long(i) + 1);
        bool seen[64] = {};
        int distinct = 0;
        for (const Hit& hit : members) {
            if (!seen[hit.index]) {
                seen[hit.index] = true;
                ++distinct;
            }
        }
        if (distinct >= 3 && members.size() >= 3) {
            clusters.push_back({members.front().address, members.back().address, distinct,
                                std::move(members)});
        }
    }
    log("Probe: %zu cluster(s) of 3+ distinct values\n", clusters.size());
    std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
        if (a.distinct != b.distinct) {
            return a.distinct > b.distinct;
        }
        return (a.last - a.first) < (b.last - b.first);
    });
    int found = 0;
    uintptr_t printed = 0;
    for (const Cluster& cluster : clusters) {
        if (cluster.first < printed + 256) {
            continue; // same record as one already printed
        }
        log("Probe: === statistics record 0x%llx..0x%llx: %d distinct value(s)\n",
            (unsigned long long)cluster.first, (unsigned long long)cluster.last, cluster.distinct);
        for (const Hit& hit : cluster.members) {
            log("Probe:     %+ld = %u\n", long(hit.address - cluster.first), values[hit.index]);
        }
        Dump(log, cluster.first - 64, 320);
        printed = cluster.first;
        if (++found >= 6) {
            break;
        }
    }
    return found;
}
/// The tightest anchor available: two numbers the pause menu showed next to each other (landed and
/// thrown of the same statistic) are usually stored next to each other too. A pair of specific
/// 32-bit values at a fixed small distance has almost no false positives in a gigabyte.
int ScanForPair(std::uint32_t first, std::uint32_t second, const std::vector<Region>& regions,
                Log& log) {
    int found = 0;
    for (const Region& region : regions) {
        const std::size_t count = std::size_t((region.end - region.start) / 4);
        const std::size_t chunk = 16384;
        for (std::size_t at = 0; at < count; at += chunk) {
            const std::size_t n = std::min(chunk, count - at);
            std::uint32_t buffer[16384];
            if (!SafeCopy(buffer, region.start + at * 4, n * 4)) {
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (buffer[i] != first) {
                    continue;
                }
                const uintptr_t address = region.start + (at + i) * 4;
                for (int words = 1; words <= 4; ++words) {
                    std::uint32_t next = 0;
                    if (!ReadWord(address + uintptr_t(words) * 4, next) || next != second) {
                        continue;
                    }
                    log("Probe: === pair %u,%u at 0x%llx (distance %d)\n", first, second,
                        (unsigned long long)address, words * 4);
                    Dump(log, address - 64, 320);
                    if (++found >= 8) {
                        return found;
                    }
                }
            }
        }
    }
    return found;
}

/// Clock values the tester read off the HUD, from broadcast_clock.txt next to bbport.ini (up to four
/// numbers on one line, e.g. seconds, milliseconds, elapsed). With them the scan can look for the
/// word that holds this value, a far sharper anchor than the countdown diff.
int TargetClocks(std::uint32_t* values, int max) {
    char path[1024] = "broadcast_clock.txt";
    if (const char* config = std::getenv("BB_CONFIG"); config && *config) {
        std::snprintf(path, sizeof(path), "%s", config);
        if (char* slash = std::strrchr(path, '/')) {
            slash[1] = '\0';
            std::snprintf(path + std::strlen(path), sizeof(path) - std::strlen(path),
                          "broadcast_clock.txt");
        } else {
            std::snprintf(path, sizeof(path), "broadcast_clock.txt");
        }
    }
    FILE* file = std::fopen(path, "r");
    if (!file) {
        return 0;
    }
    int count = 0;
    int seconds = 0;
    while (count < max && std::fscanf(file, "%d", &seconds) == 1) {
        if (seconds >= 0 && seconds <= 3600000) {
            values[count++] = std::uint32_t(seconds);
        }
    }
    std::fclose(file);
    return count;
}

/// For each word that ticked down, look for the pause menu's numbers in the 8 KiB either side. The
/// fight statistics and the round clock live in the same object, so a clock word surrounded by three
/// or more of the values read off the pause menu is the record we are after.
int ScanNearClockCandidates(const std::vector<uintptr_t>& clocks, const std::uint32_t* values,
                            int value_count, Log& log) {
    if (value_count < 2) {
        return 0;
    }
    const long radius = 8192;
    const std::size_t words = std::size_t(radius * 2) / 4;
    std::vector<std::uint32_t> window(words);
    int found = 0;
    for (uintptr_t clock : clocks) {
        if (clock < uintptr_t(radius)) {
            continue;
        }
        const uintptr_t start = clock - uintptr_t(radius);
        if (!SafeCopy(window.data(), start, words * 4)) {
            continue;
        }
        struct Hit {
            long offset;
            std::uint32_t value;
        };
        std::vector<Hit> hits;
        bool seen[8] = {};
        int distinct = 0;
        for (std::size_t i = 0; i < words; ++i) {
            const std::uint32_t word = window[i];
            if (word > 100000) {
                continue;
            }
            for (int v = 0; v < value_count; ++v) {
                if (word == values[v]) {
                    hits.push_back({long(i) * 4 - radius, word});
                    if (!seen[v]) {
                        seen[v] = true;
                        ++distinct;
                    }
                    break;
                }
            }
        }
        if (distinct < 3) {
            continue;
        }
        log("Probe: === clock 0x%llx: %d of the pause-menu value(s) within %ld bytes\n",
            (unsigned long long)clock, distinct, radius);
        int printed = 0;
        for (const Hit& hit : hits) {
            log("Probe:     %+ld = %u\n", hit.offset, hit.value);
            if (++printed >= 40) {
                log("Probe:     ...\n");
                break;
            }
        }
        if (!hits.empty()) {
            Dump(log, start + uintptr_t(hits.front().offset) - 64, 320);
        }
        if (++found >= 6) {
            break;
        }
    }
    if (found == 0) {
        log("Probe: no countdown candidate had 3+ pause-menu value(s) within %ld bytes\n", radius);
    }
    return found;
}

/// Look for the stat block by the clock's value instead of by its movement: every word that holds
/// one of the values shown on screen (or one next to it) is reported, and a projected base that
/// also passes the block test is printed in full.
int ScanForClock(const std::uint32_t* targets, int target_count, const std::vector<Region>& regions,
                 Log& log) {
    int found = 0;
    int reported = 0;
    for (const Region& region : regions) {
        const std::size_t count = std::size_t((region.end - region.start) / 4);
        const std::size_t chunk = 16384;
        for (std::size_t at = 0; at < count; at += chunk) {
            const std::size_t n = std::min(chunk, count - at);
            std::uint32_t buffer[16384];
            if (!SafeCopy(buffer, region.start + at * 4, n * 4)) {
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t word = buffer[i];
                bool hit = false;
                for (int t = 0; t < target_count && !hit; ++t) {
                    const std::int32_t target = std::int32_t(targets[t]);
                    float as_float = 0.0f;
                    std::memcpy(&as_float, &word, sizeof(as_float));
                    hit = (std::int32_t(word) >= target - 2 && std::int32_t(word) <= target + 2) ||
                          (as_float >= float(target) - 2.0f && as_float <= float(target) + 2.0f);
                }
                if (!hit) {
                    continue;
                }
                const uintptr_t address = region.start + (at + i) * 4;
                if (reported++ < 24) {
                    log("Probe:   value hit at 0x%llx (word %u, int %d)\n",
                        (unsigned long long)address, word, int(std::int32_t(word)));
                    Dump(log, address - 32, 96);
                }
                for (const int stride : {4, 8, 16}) {
                    const uintptr_t base = address - uintptr_t(IdRoundRemaining) * uintptr_t(stride);
                    if (!LooksLikeStatBlock(base, stride)) {
                        continue;
                    }
                    log("Probe: === stat block at 0x%llx (stride %d, time slot 0x%llx)\n",
                        (unsigned long long)base, stride, (unsigned long long)address);
                    PrintSlots(log, base, stride);
                    Dump(log, base, 192);
                    ++found;
                }
            }
        }
    }
    log("Probe: %d value hit(s) reported\n", reported);
    return found;
}

} // namespace

bool Enabled() {
    const char* env = std::getenv("BB_BROADCAST_PROBE");
    return env && *env && *env != '0';
}

void Scan() {
    Log log;
    const auto started = std::chrono::steady_clock::now();
    log("Probe: broadcast mod discovery scan (BB_BROADCAST_PROBE)\n");

    std::uint64_t budget = 256ull << 20;
    if (const char* env = std::getenv("BB_BROADCAST_SCAN_MB"); env && *env) {
        budget = std::uint64_t(std::max(16, std::atoi(env))) << 20;
    }
    const auto regions = MappedRegions(log);
    if (regions.empty()) {
        log("Probe: no mapped guest memory\n");
        return;
    }
    std::uint32_t values[8] = {};
    const int value_count = TargetValues(values, 8);
    if (value_count >= 2) {
        log("Probe: %d value(s) from broadcast_values.txt:", value_count);
        for (int i = 0; i < value_count; ++i) {
            log(" %u", values[i]);
        }
        log("\n");
        const auto wide_values = ScanSet(regions, 1024ull << 20, 256ull << 20, log);
        const int pairs = ScanForPair(values[0], values[1], wide_values, log);
        log("Probe: pair search (%u,%u): %d hit(s)\n", values[0], values[1], pairs);
        const int hits = ScanForValues(values, value_count, wide_values, log);
        log("Probe: value search: %d window(s)\n", hits);
    }

    std::uint32_t targets[4] = {};
    const int target_count = TargetClocks(targets, 4);
    if (target_count > 0) {
        log("Probe: %d clock value(s) from broadcast_clock.txt:", target_count);
        for (int i = 0; i < target_count; ++i) {
            log(" %u", targets[i]);
        }
        log("\n");
        const auto wide = ScanSet(regions, 1024ull << 20, 256ull << 20, log);
        const int hits = ScanForClock(targets, target_count, wide, log);
        log("Probe: clock search: %d block(s)\n", hits);
    }
    const auto set = ScanSet(regions, budget, 64ull << 20, log);

    // First pass: remember every word that could be a clock (0..600, int or float) as a 16-bit tag.
    std::vector<std::uint16_t> tags;
    std::size_t words = 0;
    for (const Region& region : set) {
        words += std::size_t((region.end - region.start) / 4);
    }
    tags.assign(words, 0xFFFF);
    {
        std::size_t index = 0;
        for (const Region& region : set) {
            const std::size_t count = std::size_t((region.end - region.start) / 4);
            const std::size_t chunk = 16384; // words (64 KiB)
            for (std::size_t at = 0; at < count; at += chunk) {
                const std::size_t n = std::min(chunk, count - at);
                std::uint32_t buffer[16384];
                if (!SafeCopy(buffer, region.start + at * 4, n * 4)) {
                    continue;
                }
                for (std::size_t i = 0; i < n; ++i) {
                    if (buffer[i] <= 600) {
                        tags[index + at + i] = std::uint16_t(buffer[i]);
                    } else {
                        float as_float = 0.0f;
                        std::memcpy(&as_float, &buffer[i], sizeof(as_float));
                        if (as_float >= 0.0f && as_float <= 600.0f) {
                            tags[index + at + i] = std::uint16_t(0x8000u | std::uint16_t(as_float));
                        }
                    }
                }
            }
            index += count;
        }
    }

    log("Probe: snapshot 1 done in %.2f s; waiting 1.5 s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Second pass: a word that fell by 1..4 (int) or ~1.5 (float) and stays in range is a clock.
    struct Candidate {
        uintptr_t address;
        std::uint32_t before, after;
    };
    std::vector<Candidate> candidates;
    {
        std::size_t index = 0;
        for (const Region& region : set) {
            const std::size_t count = std::size_t((region.end - region.start) / 4);
            const std::size_t chunk = 16384;
            for (std::size_t at = 0; at < count; at += chunk) {
                const std::size_t n = std::min(chunk, count - at);
                std::uint32_t buffer[16384];
                if (!SafeCopy(buffer, region.start + at * 4, n * 4)) {
                    continue;
                }
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uint16_t before = tags[index + at + i];
                    if (before == 0xFFFF) {
                        continue;
                    }
                    const std::uint32_t after = buffer[i];
                    bool countdown;
                    if (before & 0x8000u) {
                        const float before_float = float(before & 0x7FFFu);
                        float after_float = 0.0f;
                        std::memcpy(&after_float, &after, sizeof(after_float));
                        const float drop = before_float - after_float;
                        countdown = after_float >= 0.0f && after_float <= 600.0f && drop >= 0.5f &&
                                    drop <= 60.0f;
                    } else {
                        countdown = after <= 600 && after < before && before - after >= 1 &&
                                    before - after <= 60;
                    }
                    if (!countdown) {
                        continue;
                    }
                    candidates.push_back({region.start + (at + i) * 4, before, after});
                    if (candidates.size() > 4096) {
                        break;
                    }
                }
            }
            index += count;
        }
    }
    std::vector<uintptr_t> countdown_clocks;
    countdown_clocks.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        countdown_clocks.push_back(candidate.address);
    }
    log("Probe: %zu countdown candidate(s)\n", candidates.size());
    for (const Candidate& candidate : candidates) {
        if (candidate.before >= 20 && candidate.before - candidate.after <= 12) {
            log("Probe:   countdown 0x%llx  %u -> %u\n", (unsigned long long)candidate.address,
                candidate.before, candidate.after);
        }
    }

    int found = 0;
    std::vector<uintptr_t> seen;
    for (const Candidate& candidate : candidates) {
        for (const int stride : {4, 8, 16}) {
            const uintptr_t base =
                candidate.address - uintptr_t(IdRoundRemaining) * uintptr_t(stride);
            if (std::find(seen.begin(), seen.end(), base) != seen.end()) {
                continue;
            }
            if (!LooksLikeStatBlock(base, stride)) {
                continue;
            }
            seen.push_back(base);
            log("Probe: === stat block candidate at 0x%llx (stride %d, clock slot 0x%llx %u -> %u)\n",
                (unsigned long long)base, stride, (unsigned long long)candidate.address,
                candidate.before, candidate.after);
            PrintSlots(log, base, stride);
            Dump(log, base, 192);
            if (++found >= 8) {
                break;
            }
        }
        if (found >= 8) {
            break;
        }
    }
    if (found == 0) {
        log("Probe: no stat block under the id-indexed hypothesis\n");
    }
    const int near = ScanNearClockCandidates(countdown_clocks, values, value_count, log);
    if (near > 0) {
        found += near;
    }
    log("Probe: scan finished in %.2f s, %d block(s)\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), found);
}

// ---------------------------------------------------------------------------------------------
// Getter capture (Windows): BB_BROADCAST_CAPTURE=1 arms it, F10 in game runs it.
//
// Guest 0x1bb180 is `(context, id, out, type) -> int`: it calls [context->vtable + 0x10] and stores
// the result through `out`. Replacing its first byte (`push rbp`) with int3 and catching the trap in
// a vectored handler gives (rdi context, rdx id, rcx out) for every stat read in the game, which is
// the one thing the id-indexed scans could not provide. The handler re-executes the stolen `push rbp`
// by hand, so the game continues unchanged.
#ifdef _WIN32
#include <windows.h>

// The image is mapped at 0x800000000 + (file offset - 0x12850): 0x1bb180 in the file is 0x1ac930
// in guest memory (verified against the running game, three functions at once).
constexpr uintptr_t GetterEntry = ImageBase + 0x1ac930;
/// The integer stat read inside the getter: `mov ecx,[rax+rcx]`.
constexpr uintptr_t IntSite = ImageBase + 0x1acb77;
/// The float stat read inside the getter: `vcvttss2si ecx,[rax+rcx]`.
constexpr uintptr_t FloatSite = ImageBase + 0x1acbd7;
/// Fast scalar read (per-fighter int values): the strike/takedown/knockdown counters go
/// through this path, whose value load is `mov eax,[rcx+rax*4]`, not the loads above.
constexpr uintptr_t FastReadEntry = ImageBase + 0xb92c80;
constexpr uintptr_t FastReadSite = ImageBase + 0xb92dff;

struct CaptureEvent {
    std::uint32_t id;
    std::uint32_t side;
    std::uint64_t context;
    std::uint64_t base;
    std::int64_t offset;
    std::int64_t value;
    std::int32_t start;
    std::int32_t stride;
    std::uint64_t slot;
    bool is_fast;
};

constexpr unsigned EntryMax = 128;
constexpr unsigned ValueMax = 2048;

CaptureEvent entries[EntryMax];
CaptureEvent values[ValueMax];
std::atomic<unsigned> entry_count{0};
std::atomic<unsigned> value_count{0};
std::atomic<unsigned> capture_limit{0};
PVOID capture_handler = nullptr;
unsigned char* capture_code = nullptr;
unsigned char* capture_int_site = nullptr;
unsigned char* capture_float_site = nullptr;
unsigned char* capture_fast_entry = nullptr;
unsigned char* capture_fast_site = nullptr;
std::atomic<bool> capture_armed{false};
Log* capture_log = nullptr;
// The fast-read entry trap stashes (id, side, object): its value load only sees the index,
// so the load trap pairs the read back to the id through these.
std::atomic<std::uint32_t> fast_id{0};
std::atomic<std::uint32_t> fast_side{0};
std::atomic<std::uint64_t> fast_object{0};
std::atomic<bool> fast_pending{false};

/// Puts the patched bytes back and unhooks. Safe from the trap handler: it never runs them.
void DisarmCapture() {
    if (!capture_armed.exchange(false)) {
        return;
    }
    auto restore = [](unsigned char* site, unsigned char byte) {
        if (!site) {
            return;
        }
        DWORD old = 0;
        if (VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &old)) {
            *site = byte;
            VirtualProtect(site, 16, old, &old);
            FlushInstructionCache(GetCurrentProcess(), site, 1);
        }
    };
    restore(capture_code, 0x55);
    restore(capture_int_site, 0x8b);
    restore(capture_float_site, 0xc5);
    restore(capture_fast_entry, 0x53);
    restore(capture_fast_site, 0x8b);
    if (capture_handler) {
        RemoveVectoredExceptionHandler(capture_handler);
        capture_handler = nullptr;
    }
    if (capture_log) {
        (*capture_log)("Probe: getter capture disarmed\n");
    }
}

void CommitRead(const CaptureEvent& event);

/// Records one stat read: `base + offset` is where the game just loaded the value from.
/// One row per stat cell: a cell read again on a later frame only refreshes its value.
void RecordRead(const CONTEXT* context, bool is_float) {
    CaptureEvent event{};
    event.id = static_cast<std::uint32_t>(context->R12);
    event.side = static_cast<std::uint32_t>(context->R14);
    event.base = context->Rax;
    event.offset = static_cast<std::int64_t>(context->Rcx);
    event.slot = context->Rax + context->Rcx;
    std::int32_t local = 0;
    if (SafeCopy(&local, context->Rbp - 0x48, sizeof(local))) {
        event.start = local;
    }
    if (SafeCopy(&local, context->Rbp - 0x40, sizeof(local))) {
        event.stride = local;
    }
    if (is_float) {
        float raw = 0.0f;
        if (SafeCopy(&raw, event.slot, sizeof(raw))) {
            event.value = static_cast<std::int64_t>(raw);
        }
    } else {
        std::int32_t raw = 0;
        if (SafeCopy(&raw, event.slot, sizeof(raw))) {
            event.value = raw;
        }
    }
    CommitRead(event);
}

/// Appends or refreshes one value record (deduplicated by id + side + cell).
void CommitRead(const CaptureEvent& event) {
    const unsigned total = std::min(value_count.load(), ValueMax);
    for (unsigned i = 0; i < total; ++i) {
        CaptureEvent& known = values[i];
        if (known.id == event.id && known.side == event.side && known.slot == event.slot) {
            known.value = event.value;
            return;
        }
    }
    const unsigned index = value_count.fetch_add(1);
    if (index < ValueMax) {
        values[index] = event;
    }
}

/// Records a fast-path scalar read: the value is at `data + index*4`; the id and side were
/// stashed by the fast-read entry trap right before (the load itself only sees the index).
void RecordFastRead(const CONTEXT* context) {
    if (!fast_pending.load(std::memory_order_relaxed)) {
        return;
    }
    CaptureEvent event{};
    event.is_fast = true;
    event.id = fast_id.load(std::memory_order_relaxed);
    event.side = fast_side.load(std::memory_order_relaxed);
    event.context = fast_object.load(std::memory_order_relaxed);
    event.base = context->Rcx;
    event.offset = static_cast<std::int64_t>(context->Rax) * 4;
    event.slot = context->Rcx + context->Rax * 4;
    event.start = 0;
    event.stride = 4;
    std::int32_t raw = 0;
    if (SafeCopy(&raw, event.slot, sizeof(raw))) {
        event.value = raw;
    }
    CommitRead(event);
}

LONG CALLBACK OnCaptureTrap(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    CONTEXT* context = info->ContextRecord;
    if (address == GetterEntry) {
        const unsigned index = entry_count.fetch_add(1);
        if (index < EntryMax) {
            entries[index].id = static_cast<std::uint32_t>(context->Rsi);
            entries[index].side = static_cast<std::uint32_t>(context->Rdx);
            entries[index].context = context->Rdi;
        }
        // The breakpoint replaced `push rbp`: do it by hand and resume after it.
        context->Rsp -= 8;
        *reinterpret_cast<std::uint64_t*>(context->Rsp) = context->Rbp;
        context->Rip = GetterEntry + 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == IntSite) {
        std::int32_t raw = 0;
        SafeCopy(&raw, context->Rax + context->Rcx, sizeof(raw));
        RecordRead(context, false);
        context->Rcx = static_cast<std::uint32_t>(raw);
        context->Rip = IntSite + 3;
        if (value_count.load() >= capture_limit.load()) {
            DisarmCapture();
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == FloatSite) {
        float raw = 0.0f;
        std::int32_t converted = 0;
        if (SafeCopy(&raw, context->Rax + context->Rcx, sizeof(raw))) {
            converted = static_cast<std::int32_t>(raw);
        }
        RecordRead(context, true);
        context->Rcx = static_cast<std::uint32_t>(converted);
        context->Rip = FloatSite + 5;
        if (value_count.load() >= capture_limit.load()) {
            DisarmCapture();
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == FastReadEntry) {
        fast_id.store(static_cast<std::uint32_t>(context->Rsi), std::memory_order_relaxed);
        fast_side.store(static_cast<std::uint32_t>(context->Rcx), std::memory_order_relaxed);
        fast_object.store(context->Rdi, std::memory_order_relaxed);
        fast_pending.store(true, std::memory_order_relaxed);
        // The breakpoint replaced `push rbx`: do it by hand and resume after it.
        context->Rsp -= 8;
        *reinterpret_cast<std::uint64_t*>(context->Rsp) = context->Rbx;
        context->Rip = FastReadEntry + 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == FastReadSite) {
        std::int32_t raw = 0;
        SafeCopy(&raw, context->Rcx + context->Rax * 4, sizeof(raw));
        RecordFastRead(context);
        context->Rax = static_cast<std::uint32_t>(raw); // `mov eax,[rcx+rax*4]` zero-extends
        context->Rip = FastReadSite + 3;
        if (value_count.load() >= capture_limit.load()) {
            DisarmCapture();
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// Reports the captured reads: id, side, the address the value came from and the value itself.
void FlushCaptures(Log& log) {
    const unsigned entry_total = std::min(entry_count.load(), EntryMax);
    log("Probe: getter entry: %u call(s)\n", entry_total);
    std::uint64_t contexts[4] = {};
    int context_count = 0;
    for (unsigned i = 0; i < entry_total; ++i) {
        const CaptureEvent& event = entries[i];
        log("Probe:   entry id %u side %u  context 0x%llx\n", event.id, event.side,
            (unsigned long long)event.context);
        if (context_count < 4 && event.context &&
            std::find(std::begin(contexts), std::end(contexts), event.context) == std::end(contexts)) {
            contexts[context_count++] = event.context;
        }
    }
    const unsigned value_total = std::min(value_count.load(), ValueMax);
    log("Probe: value read: %u call(s)\n", value_total);
    for (unsigned i = 0; i < value_total; ++i) {
        const CaptureEvent& event = values[i];
        log("Probe:   read id %u side %u  base 0x%llx off %lld -> 0x%llx = %lld  start %d stride %d%s\n",
            event.id, event.side, (unsigned long long)event.base, (long long)event.offset,
            (unsigned long long)event.slot, (long long)event.value, event.start, event.stride,
            event.is_fast ? "  [fast]" : "");
    }
    for (int index = 0; index < context_count; ++index) {
        const std::uint64_t context = contexts[index];
        std::uint8_t dump[0x200] = {};
        if (!SafeCopy(dump, context, sizeof(dump))) {
            log("Probe: context 0x%llx unreadable\n", (unsigned long long)context);
            continue;
        }
        log("Probe: context 0x%llx (first 0x200 bytes, qword view)\n", (unsigned long long)context);
        for (std::size_t row = 0; row < sizeof(dump); row += 32) {
            std::uint64_t words[4] = {};
            std::memcpy(words, dump + row, sizeof(words));
            log("Probe:   +0x%03zx  0x%016llx 0x%016llx 0x%016llx 0x%016llx\n", row,
                (unsigned long long)words[0], (unsigned long long)words[1],
                (unsigned long long)words[2], (unsigned long long)words[3]);
        }
    }
}

/// Arms the capture: F10 in game (BB_BROADCAST_CAPTURE=1), then the next stat reads are recorded.
void ArmCapture() {
    if (capture_armed.exchange(true)) {
        return;
    }
    static Log log;
    capture_log = &log;
    capture_code = reinterpret_cast<unsigned char*>(GetterEntry);
    capture_int_site = reinterpret_cast<unsigned char*>(IntSite);
    capture_float_site = reinterpret_cast<unsigned char*>(FloatSite);
    capture_fast_entry = reinterpret_cast<unsigned char*>(FastReadEntry);
    capture_fast_site = reinterpret_cast<unsigned char*>(FastReadSite);
    DWORD old = 0;
    struct Site {
        unsigned char* address;
        unsigned char expected;
        unsigned char patch;
        const char* name;
    } sites[5] = {
        {capture_code, 0x55, 0xCC, "getter"},
        {capture_int_site, 0x8b, 0xCC, "int read"},
        {capture_float_site, 0xc5, 0xCC, "float read"},
        {capture_fast_entry, 0x53, 0xCC, "fast read entry"},
        {capture_fast_site, 0x8b, 0xCC, "fast read load"},
    };
    for (Site& site : sites) {
        if (!VirtualProtect(site.address, 16, PAGE_EXECUTE_READWRITE, &old)) {
            log("Probe: getter capture: cannot write guest code (%lu)\n", GetLastError());
            capture_armed = false;
            return;
        }
        if (*site.address != site.expected) {
            log("Probe: getter capture: %s site at 0x%llx starts with 0x%02x, expected 0x%02x\n",
                site.name, (unsigned long long)reinterpret_cast<std::uintptr_t>(site.address),
                *site.address, site.expected);
            VirtualProtect(site.address, 16, old, &old);
            capture_armed = false;
            return;
        }
    }
    entry_count = 0;
    value_count = 0;
    capture_limit = ValueMax;
    capture_handler = AddVectoredExceptionHandler(1, OnCaptureTrap);
    if (!capture_handler) {
        log("Probe: getter capture: no exception handler (%lu)\n", GetLastError());
        capture_armed = false;
        return;
    }
    for (Site& site : sites) {
        *site.address = site.patch;
        FlushInstructionCache(GetCurrentProcess(), site.address, 1);
    }
    log("Probe: getter capture armed at 0x%llx + fast read 0x%llx, %u reads\n",
        (unsigned long long)GetterEntry, (unsigned long long)FastReadEntry, ValueMax);
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        DisarmCapture();
        FlushCaptures(*capture_log);
    }).detach();
}
#else
void ArmCapture() {}
void DisarmCapture() {}
#endif

void Request() {
    // Its own thread: the scan sleeps 1.5 s between the two passes and must not stall a frame.
    std::thread([] { Scan(); }).detach();
}

void Arm() {
    ArmCapture();
}

} // namespace BroadcastProbe
