// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: broadcast overlay mod, Phase 2. Live fight statistics.
//
// The game reads its own statistics by stat id through two paths (docs/ufc3/BROADCAST-MOD-PHASE0.md
// §3.4.1 and BROADCAST-MOD-PHASE2.md stage A):
//
//   1. a *fast scalar read* at guest 0xb92c80, which the per-fighter strike, takedown and knockdown
//      counters use. Its first argument (rdi) is the per-match stat store: sha-buckets at +0x10, the
//      bucket count at +0x18, and one node per id whose +0x8 points at a value descriptor
//      (`values` at +0x18, element count at +0x10, per-dimension strides at +0x8).
//   2. the general getter at guest 0x1ac930, whose integer load we trap to learn where a value lives
//      (`base + offset`). Head and overall health come through here.
//
// Stage B reads values the way the game does instead of hardcoding addresses: a short capture at
// fight start learns the stat store pointer and the health cells, then every value is a plain
// guarded read from the store's own arrays. Nothing stays patched: the capture is armed for at most
// two seconds, disarms itself, and is armed again only when the cached cells stop looking right
// (cells move between fights — the arena is re-allocated).
#include "broadcast_stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "bbport_toggles.h" // runtime_fault_recover, bb_fault_jmp_buf

extern "C" int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                     int* mapped);

namespace Broadcast {
namespace Stats {
namespace {

// The stat ids, taken from the dictionary in eboot.bin (BROADCAST-MOD-PHASE0.md §3.2 and the
// Phase 2 stage A table). FIGHTER_STRIKES_LANDED (3152) and BETWEEN_ROUND_STATE (6714) were read out
// of the dictionary's own initialiser for this phase.
enum : int {
    IdTotalStrikesLanded = 3152,
    IdSigLanded = 7566,
    IdSigThrown = 9567,
    IdHeadshotsLanded = 8850,
    IdHeadshotsThrown = 7077,
    IdTotalStrikesThrown = 9391,
    IdTakedownsLanded = 5685,
    IdTakedownsAttempted = 9343,
    IdKnockedDown = 1683,
    IdHeadHealth = 8851,
    IdOverallHealth = 8927,
    IdRoundNumber = 9910,
    IdRoundRemaining = 4454,
    IdMatchState = 6716,
    IdBetweenRounds = 6714,
};

/// A read a fault cannot kill: the runtime's guest-read recovery resumes it here. The same helper
/// the discovery probe uses, so a stale cell can never take the game down.
bool SafeCopy(void* destination, std::uintptr_t source, std::size_t size) {
    if (source == 0) {
        return false;
    }
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

template <typename T>
bool Read(std::uintptr_t address, T& value) {
    return SafeCopy(&value, address, sizeof(T));
}

double Now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point started = clock::now();
    return std::chrono::duration<double>(clock::now() - started).count();
}

// -------------------------------------------------------------------------------------------
// The capture: two int3 sites, a vectored handler, at most two seconds.
// -------------------------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>

constexpr std::uintptr_t ImageBase = 0x800000000ull;
/// Fast scalar read entry: `push rbx`, rdi = stat store, esi = id, ecx = number of dimensions.
constexpr std::uintptr_t FastEntry = ImageBase + 0xb92c80;
/// The integer stat load inside the general getter: `mov ecx,[rax+rcx]` (id in r12).
constexpr std::uintptr_t IntLoad = ImageBase + 0x1acb77;
/// The float stat load inside the general getter: `vcvttss2si ecx,[rax+rcx]`. Head and overall
/// health are stored as floats and come through here.
constexpr std::uintptr_t FloatLoad = ImageBase + 0x1acbd7;

constexpr int MaxCells = 96;
constexpr int IdSpan = 16384;

/// One learned cell: the base the game loaded from and the byte offset it applied.
struct Learned {
    std::uint32_t id = 0;
    std::uint64_t base = 0;
    std::int32_t offset = 0;
};

Learned learned[MaxCells];
std::atomic<int> learnedCount{0};
std::atomic<std::uint64_t> statStore{0};
std::atomic<int> seenIds{0};

PVOID trapHandler = nullptr;
std::atomic<bool> armed{false};
/// Set when a capture starts, consumed when it closes: the signal to turn its reads into cells.
std::atomic<bool> captureFresh{false};

void RestoreBytes(unsigned char* site, unsigned char byte) {
    if (!site) {
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &old)) {
        *site = byte;
        VirtualProtect(site, 16, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 1);
    }
}

void Disarm() {
    if (!armed.exchange(false)) {
        return;
    }
    RestoreBytes(reinterpret_cast<unsigned char*>(FastEntry), 0x53);
    RestoreBytes(reinterpret_cast<unsigned char*>(IntLoad), 0x8b);
    RestoreBytes(reinterpret_cast<unsigned char*>(FloatLoad), 0xc5);
    if (trapHandler) {
        RemoveVectoredExceptionHandler(trapHandler);
        trapHandler = nullptr;
    }
    std::printf("Broadcast: stat capture off (store %d, ids %d)\n", statStore.load() ? 1 : 0,
                int(seenIds.load()));
}

/// Records a learned cell. Shared by the handler.
void Learn(std::uint32_t id, std::uint64_t base, std::int32_t offset) {
    if (id == 0 || id >= IdSpan || base == 0) {
        return;
    }
    const int total = std::min(learnedCount.load(), MaxCells);
    for (int i = 0; i < total; ++i) {
        if (learned[i].id == id && learned[i].base == base) {
            learned[i].offset = offset;
            return;
        }
    }
    const int index = learnedCount.fetch_add(1);
    if (index < MaxCells) {
        learned[index].id = id;
        learned[index].base = base;
        learned[index].offset = offset;
    }
    seenIds.fetch_add(1);
}

LONG CALLBACK OnTrap(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    CONTEXT* context = info->ContextRecord;

    if (address == FastEntry) {
        // The store only has to be learned once; the first fast read of a fight hands it over.
        if (statStore.load() == 0 && context->Rdi != 0) {
            statStore.store(context->Rdi);
        }
        context->Rsp -= 8;
        *reinterpret_cast<std::uint64_t*>(context->Rsp) = context->Rbx;
        context->Rip = FastEntry + 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == IntLoad) {
        std::int32_t raw = 0;
        SafeCopy(&raw, context->Rax + context->Rcx, sizeof(raw));
        Learn(static_cast<std::uint32_t>(context->R12), context->Rax,
              static_cast<std::int32_t>(context->Rcx));
        context->Rcx = static_cast<std::uint64_t>(static_cast<std::uint32_t>(raw));
        context->Rip = IntLoad + 3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (address == FloatLoad) {
        float raw = 0.0f;
        std::int32_t converted = 0;
        if (SafeCopy(&raw, context->Rax + context->Rcx, sizeof(raw))) {
            converted = static_cast<std::int32_t>(raw);
        }
        Learn(static_cast<std::uint32_t>(context->R12), context->Rax,
              static_cast<std::int32_t>(context->Rcx));
        context->Rcx = static_cast<std::uint64_t>(static_cast<std::uint32_t>(converted));
        context->Rip = FloatLoad + 5;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void Arm() {
    if (armed.exchange(true)) {
        return;
    }
    unsigned char* fast = reinterpret_cast<unsigned char*>(FastEntry);
    unsigned char* load = reinterpret_cast<unsigned char*>(IntLoad);
    unsigned char* floatLoad = reinterpret_cast<unsigned char*>(FloatLoad);
    DWORD old = 0;
    if (!VirtualProtect(fast, 16, PAGE_EXECUTE_READWRITE, &old)) {
        armed.store(false);
        return;
    }
    VirtualProtect(load, 16, PAGE_EXECUTE_READWRITE, &old);
    VirtualProtect(floatLoad, 16, PAGE_EXECUTE_READWRITE, &old);
    if (*fast != 0x53 || *load != 0x8b || *floatLoad != 0xc5) {
        VirtualProtect(fast, 16, old, &old);
        VirtualProtect(load, 16, old, &old);
        VirtualProtect(floatLoad, 16, old, &old);
        armed.store(false);
        std::printf("Broadcast: stat capture: unexpected code at the trap sites, stats off\n");
        return;
    }
    statStore.store(0);
    learnedCount.store(0);
    seenIds.store(0);
    captureFresh.store(true);
    trapHandler = AddVectoredExceptionHandler(1, OnTrap);
    if (!trapHandler) {
        armed.store(false);
        std::printf("Broadcast: stat capture: no exception handler, stats off\n");
        return;
    }
    *fast = 0xCC;
    *load = 0xCC;
    *floatLoad = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), fast, 1);
    FlushInstructionCache(GetCurrentProcess(), load, 1);
    FlushInstructionCache(GetCurrentProcess(), floatLoad, 1);
    // Bounded, and never left behind: two seconds is the whole window (handoff §4).
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Disarm();
    }).detach();
}
#else
void Arm() {}
void Disarm() {}
std::atomic<bool> armed{false};
std::atomic<bool> captureFresh{false};
struct Learned {
    std::uint32_t id = 0;
    std::uint64_t base = 0;
    std::int32_t offset = 0;
};
Learned learned[96];
std::atomic<int> learnedCount{0};
std::atomic<std::uint64_t> statStore{0};
std::atomic<int> seenIds{0};
#endif

// -------------------------------------------------------------------------------------------
// The cache: what one fight's cells are, and how to read a statistic out of them.
// -------------------------------------------------------------------------------------------

/// The store's descripton of one id's array, re-resolved on every use so a re-allocated arena is
/// noticed instead of read.
struct Array {
    bool ok = false;
    std::uintptr_t values = 0;
    int count = 0;
    std::uintptr_t strides = 0;
    int dimensions = 0;
};

Array Lookup(std::uintptr_t store, int id, int dimensions) {
    Array array;
    if (store == 0) {
        return array;
    }
    std::uint64_t buckets = 0;
    std::uint64_t count64 = 0;
    if (!Read(store + 0x10, buckets) || !Read(store + 0x18, count64)) {
        return array;
    }
    const std::uint32_t bucketCount = static_cast<std::uint32_t>(count64);
    if (buckets == 0 || bucketCount == 0 || bucketCount > (1u << 20)) {
        return array;
    }
    const std::uint32_t bucket = static_cast<std::uint32_t>(id) % bucketCount;
    std::uint64_t node = 0;
    if (!Read(buckets + std::uintptr_t(bucket) * 8, node)) {
        return array;
    }
    for (int guard = 0; node != 0 && guard < 64; ++guard) {
        std::uint32_t nodeId = 0;
        if (!Read(node, nodeId)) {
            return array;
        }
        if (nodeId == static_cast<std::uint32_t>(id)) {
            std::uint64_t descriptor = 0;
            if (!Read(node + 0x8, descriptor) || descriptor == 0) {
                return array;
            }
            std::uint32_t elements = 0;
            std::uint64_t values = 0;
            std::uint64_t strides = 0;
            if (!Read(descriptor + 0x10, elements) || !Read(descriptor + 0x18, values) ||
                !Read(descriptor + 0x8, strides)) {
                return array;
            }
            if (values == 0 || strides == 0 || elements == 0 || elements > 8192) {
                return array;
            }
            array.ok = true;
            array.values = values;
            array.count = static_cast<int>(elements);
            array.strides = strides;
            array.dimensions = dimensions;
            return array;
        }
        if (!Read(node + 0x10, node)) {
            return array;
        }
    }
    return array;
}

/// The element index the game itself would compute: the dot product of the coordinates and the
/// per-dimension strides. `slot` is the fighter (0 away, 1 home), `round` 0 is the fight total.
bool IndexOf(const Array& array, int slot, int round, int& index) {
    if (!array.ok || array.dimensions < 2 || array.dimensions > 3) {
        return false;
    }
    const int coordinates[3] = {slot, round, 0};
    int total = 0;
    for (int i = 0; i < array.dimensions; ++i) {
        std::int32_t stride = 0;
        if (!Read(array.strides + std::uintptr_t(i) * 4, stride) || stride < 0) {
            return false;
        }
        total += coordinates[i] * stride;
    }
    if (total < 0 || total >= array.count) {
        return false;
    }
    index = total;
    return true;
}

bool ArrayValue(const Array& array, int slot, int round, int& value) {
    int index = 0;
    if (!IndexOf(array, slot, round, index)) {
        return false;
    }
    std::int32_t raw = 0;
    if (!Read(array.values + std::uintptr_t(index) * 4, raw)) {
        return false;
    }
    value = raw;
    return true;
}

/// Which sub-slot of a strike-type row to read: the last one (the game's row total) or the whole row.
constexpr int kRowLast = -1;
constexpr int kRowSum = -2;

/// A statistic whose array carries a trailing strike-type dimension (total strikes landed, takedowns
/// landed). `pick` chooses which sub-slot is wanted: kRowLast takes the last sub-slot (the game's own
/// row total), kRowSum adds the whole row, anything else takes that slot (0 = first). The strides say
/// how many sub-slots there are and how far apart, so nothing is hardcoded.
bool ArraySub(const Array& array, int slot, int round, int pick, int& value) {
    if (array.dimensions < 3) {
        return ArrayValue(array, slot, round, value);
    }
    std::int32_t strides[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!Read(array.strides + std::uintptr_t(i) * 4, strides[i])) {
            return false;
        }
    }
    if (strides[0] <= 0 || strides[1] <= 0 || strides[2] <= 0) {
        return false;
    }
    const int base = slot * strides[0] + round * strides[1];
    const int tail = strides[1] / strides[2];
    if (tail <= 0 || tail > 64) {
        return false;
    }
    int first = 0;
    int last = 0;
    if (pick == kRowSum) {
        first = 0;
        last = tail - 1;
    } else if (pick == kRowLast || pick >= tail) {
        first = tail - 1;
        last = first;
    } else {
        first = std::max(pick, 0);
        last = first;
    }
    int total = 0;
    for (int k = first; k <= last; ++k) {
        const int at = base + k * strides[2];
        if (at < 0 || at >= array.count) {
            return false;
        }
        std::int32_t raw = 0;
        if (!Read(array.values + std::uintptr_t(at) * 4, raw)) {
            return false;
        }
        total += raw;
    }
    value = total;
    return true;
}

/// Which statistics need how many dimensions in the store's own arrays (stage A measured them:
/// the counters read through the fast path are 2-dimensional, the two aggregate arrays are 3-).
struct StatSpec {
    int id;
    int dimensions;
};

constexpr StatSpec TotalStrikesLanded{IdTotalStrikesLanded, 3};
constexpr StatSpec SigLanded{IdSigLanded, 2};
constexpr StatSpec SigThrown{IdSigThrown, 2};
constexpr StatSpec TakedownsLanded{IdTakedownsLanded, 3};
constexpr StatSpec TakedownsAttempted{IdTakedownsAttempted, 2};
constexpr StatSpec KnockedDown{IdKnockedDown, 2};

/// Everything one fight's cache holds. All-zero until a capture has validated.
struct Cache {
    bool valid = false;
    std::uintptr_t store = 0;

    std::uintptr_t round = 0;
    std::uintptr_t remaining = 0;
    std::uintptr_t matchState = 0;
    std::uintptr_t betweenRounds = 0;

    /// Health arrays are 1-dimensional: the same base with the away fighter at +4 and home at +8.
    bool healthKnown = false;
    std::uintptr_t headHealthBlock = 0;
    std::uintptr_t overallHealthBlock = 0;

    int roundValue = 0;
    int remainingValue = 0;
    int matchStateValue = 0;
    int betweenRoundsValue = 0;
};

Cache cache;
double nextCaptureAt = 0.0;
/// Short captures taken so far while hunting the health cells (they are read only when the game
/// draws a fighter's damage). Reset when a new fight's cells are learned.
int healthProbes = 0;
bool bellSeen = false;
double lastClockChangeAt = 0.0;
int lastClockValue = -1;
/// The bell only counts once this fight's clock has actually run: a walk-in clock of 0 is not one.
bool sawRunningClock = false;
const char* status = "no capture yet";
bool statusLogged = false;
std::uint64_t knockdownSerial = 0;
Knockdown lastKnockdown;
int knockdownSeen[2] = {0, 0};
bool knockdownBaseline = false;

void SetStatus(const char* text) {
    status = text;
    if (!statusLogged) {
        statusLogged = true;
        std::printf("Broadcast: stats: %s\n", text);
    }
}

// Keeps the compiler honest about the unused helpers on the Linux build.
void UnusedHelpers() {
    (void)Disarm;
}

/// Which learned base a stat id lives at, if the capture saw it.
bool LearnedBase(int id, std::uintptr_t& base) {
    const int total = std::min(learnedCount.load(), 96);
    for (int i = 0; i < total; ++i) {
        if (learned[i].id == static_cast<std::uint32_t>(id) && learned[i].base != 0) {
            base = static_cast<std::uintptr_t>(learned[i].base);
            return true;
        }
    }
    return false;
}

/// Re-learns the cells from the last capture. Everything is validated before it is trusted.
bool Rebuild() {
    const std::uintptr_t store = static_cast<std::uintptr_t>(statStore.load());
    if (store == 0) {
        SetStatus("the capture saw no stat store");
        return false;
    }
    Cache fresh;
    fresh.store = store;
    if (!LearnedBase(IdRoundNumber, fresh.round) || !LearnedBase(IdRoundRemaining, fresh.remaining)) {
        SetStatus("the capture saw no round clock");
        return false;
    }
    LearnedBase(IdMatchState, fresh.matchState);
    LearnedBase(IdBetweenRounds, fresh.betweenRounds);

    std::uintptr_t head = 0;
    std::uintptr_t overall = 0;
    const bool trappedHealth =
        LearnedBase(IdHeadHealth, head) && LearnedBase(IdOverallHealth, overall);
    // The pause-menu stats page is the only screen that reads the health ids, so a capture taken in
    // ordinary play usually misses them. The store knows them anyway: same hash, same value arrays.
    if (!trappedHealth) {
        const Array headArray = Lookup(store, IdHeadHealth, 1);
        const Array overallArray = Lookup(store, IdOverallHealth, 1);
        if (headArray.ok && overallArray.ok) {
            head = headArray.values;
            overall = overallArray.values;
        }
    }
    fresh.healthKnown = head != 0 && overall != 0;
    fresh.headHealthBlock = head;
    fresh.overallHealthBlock = overall;

    // The store has to know the per-fighter counters, or this is not a fight's store.
    if (!Lookup(store, IdSigLanded, 2).ok || !Lookup(store, IdSigThrown, 2).ok) {
        SetStatus("the capture's stat store has no strike counters");
        return false;
    }
    std::int32_t round = 0;
    std::int32_t remaining = 0;
    if (!Read(fresh.round, round) || !Read(fresh.remaining, remaining)) {
        SetStatus("the captured clock cells are unreadable");
        return false;
    }
    if (round < 1 || round > 5 || remaining < 0 || remaining > 900) {
        // The title screen and the menus answer with round 0: that is not a fight, keep looking.
        SetStatus("the captured clock cells do not look like a round");
        return false;
    }
    cache = fresh;
    cache.valid = true;
    healthProbes = 0;
    status = nullptr;
    statusLogged = false;
    lastClockValue = -1;
    lastClockChangeAt = Now();
    sawRunningClock = false;
    knockdownBaseline = false;
    // One line per array, so the layout the card relies on is in the log and not guessed.
    const StatSpec specs[6] = {TotalStrikesLanded, SigLanded,  SigThrown,
                               TakedownsLanded,    TakedownsAttempted, KnockedDown};
    char layout[256];
    int used = 0;
    for (const StatSpec& spec : specs) {
        const Array array = Lookup(store, spec.id, spec.dimensions);
        std::int32_t first = 0;
        std::int32_t second = 0;
        if (array.ok) {
            Read(array.strides, first);
            Read(array.strides + 4, second);
        }
        used += std::snprintf(layout + used, sizeof(layout) - std::size_t(used),
                              " %d:%s n=%d s=%d,%d", spec.id, array.ok ? "ok" : "-", array.count,
                              int(first), int(second));
        if (used >= int(sizeof(layout)) - 40) {
            break;
        }
    }
    std::printf("Broadcast: stats cached (store 0x%llx, round 0x%llx, clock 0x%llx, %s health)%s\n",
                (unsigned long long)cache.store, (unsigned long long)cache.round,
                (unsigned long long)cache.remaining, cache.healthKnown ? "with" : "without",
                layout);
    return true;
}

int Cell(std::uintptr_t address) {
    std::int32_t value = 0;
    if (!Read(address, value)) {
        return -1;
    }
    return value;
}

/// Reads one fighter's health: away is +4, home +8 off the learned block. Health is stored as a
/// float, and an unwritten cell reads -1, so both readings are tried and the plausible one wins.
bool Health(std::uintptr_t block, bool home, int& value) {
    if (!cache.healthKnown || block == 0) {
        return false;
    }
    std::int32_t raw = 0;
    if (!Read(block + (home ? 8 : 4), raw)) {
        return false;
    }
    float asFloat = 0.0f;
    std::memcpy(&asFloat, &raw, sizeof(asFloat));
    int result = -1;
    // Head health is a float, overall health an int: a tiny float (a plain small int read as float
    // is a denormal) never counts as a percentage, so the int reading gets its turn.
    if (asFloat >= 0.5f && asFloat <= 200.0f) {
        result = int(asFloat + 0.5f);
    } else if (raw >= 0 && raw <= 200) {
        result = raw;
    }
    if (result < 0) {
        return false;
    }
    value = std::min(result, 100);
    return true;
}

/// One fighter's numbers for one round (round 0 is the fight total the game keeps itself).
void ReadFighter(int slot, int round, bool home, Fighter& out) {
    const std::uintptr_t store = cache.store;
    int value = 0;
    if (ArrayValue(Lookup(store, IdSigLanded, 2), slot, round, value)) {
        out.sigLanded = std::max(value, 0);
    }
    if (ArrayValue(Lookup(store, IdSigThrown, 2), slot, round, value)) {
        out.sigThrown = std::max(value, 0);
    }
    // Total strikes landed keeps a trailing strike-type dimension whose last slot is the row total the
    // game prints (measured: 21 + 3 + 2 + 1 -> 27, which is what the pause menu showed). The
    // significant count stands in when the array is not there.
    out.totalLanded = out.sigLanded;
    if (ArraySub(Lookup(store, IdTotalStrikesLanded, 3), slot, round, kRowLast, value)) {
        out.totalLanded = std::max(value, 0);
    }
    // Takedowns keep their landed count in the first sub-slot (measured: 4 landed, 4 attempted in the
    // game's page, store row "4 0 2 0 0 0 0"): the rest of the row is not a strike-type split.
    if (ArraySub(Lookup(store, IdTakedownsLanded, 3), slot, round, 0, value)) {
        out.takedownsLanded = std::max(value, 0);
    }
    if (ArrayValue(Lookup(store, IdTakedownsAttempted, 2), slot, round, value)) {
        out.takedownsAttempted = std::max(value, 0);
    }
    if (ArrayValue(Lookup(store, IdKnockedDown, 2), slot, round, value)) {
        out.knockedDown = std::max(value, 0);
    }
    if (!Health(cache.headHealthBlock, home, value)) {
        value = 100;
    }
    out.headHealth = value;
    if (!Health(cache.overallHealthBlock, home, value)) {
        value = 100;
    }
    out.overallHealth = value;
}

/// The knockdown cell is a fight total in the game's own layout; it is what the popup watches.
void PollKnockdowns() {
    if (!cache.valid) {
        return;
    }
    const std::uintptr_t store = cache.store;
    int away = 0;
    int home = 0;
    const Array array = Lookup(store, IdKnockedDown, 2);
    if (!ArrayValue(array, 0, 0, away) || !ArrayValue(array, 1, 0, home)) {
        return;
    }
    // A cell that was never written reads -1: only a real, sane count is an event.
    away = std::clamp(away, 0, 20);
    home = std::clamp(home, 0, 20);
    if (!knockdownBaseline) {
        // The first read after a capture is the starting point, not an event.
        knockdownSeen[0] = away;
        knockdownSeen[1] = home;
        knockdownBaseline = true;
        return;
    }
    const int beforeAway = knockdownSeen[0];
    const int beforeHome = knockdownSeen[1];
    knockdownSeen[0] = away;
    knockdownSeen[1] = home;
    if (home > beforeHome && home > 0) {
        lastKnockdown.kind = KnockdownKind::HomeDropped;
        lastKnockdown.count = home;
        lastKnockdown.serial = ++knockdownSerial;
        std::printf("Broadcast: knockdown, home fighter down (%d)\n", home);
    } else if (away > beforeAway && away > 0) {
        lastKnockdown.kind = KnockdownKind::AwayDropped;
        lastKnockdown.count = away;
        lastKnockdown.serial = ++knockdownSerial;
        std::printf("Broadcast: knockdown, away fighter down (%d)\n", away);
    }
}

} // namespace

void Init() {
#ifdef _WIN32
    // Nothing to read yet: the first Tick() inside a fight arms the short capture.
#else
    std::printf("Broadcast: stats need Windows for now\n");
    SetStatus("stats need Windows for now");
#endif
}

void Tick() {
#ifdef _WIN32
    const double now = Now();

    // A cache only lives as long as the fight it was captured in: the arena moves between fights.
    if (cache.valid) {
        const int round = Cell(cache.round);
        const int remaining = Cell(cache.remaining);
        if (round < 0 || remaining < 0) {
            cache.valid = false;
            SetStatus("the cached cells went away");
        } else {
            cache.roundValue = round;
            cache.remainingValue = remaining;
            cache.matchStateValue = Cell(cache.matchState);
            cache.betweenRoundsValue = cache.betweenRounds ? Cell(cache.betweenRounds) : 0;
            if (round < 1) {
                // The fight is over and the game went back to its menus: the next fight has its own
                // arena, so drop the cells and learn them again.
                cache.valid = false;
                SetStatus("the fight ended");
                return;
            }
            if (remaining != lastClockValue) {
                lastClockValue = remaining;
                lastClockChangeAt = now;
                if (remaining > 5) {
                    sawRunningClock = true;
                    bellSeen = false;
                } else if (sawRunningClock && remaining <= 2) {
                    // The bell: a new fight may start soon, so the cells have to be re-learned.
                    bellSeen = true;
                }
            }
        }
    }

    // When to learn the cells again. Every capture is short (two seconds) and never a running
    // frame: nothing stays patched while the fight is being played.
    bool wantCapture = false;
    bool healthProbe = false;
    if (!cache.valid) {
        wantCapture = true;
    } else if (bellSeen) {
        // The bell rang: the next fight has its own arena, so re-learn before it starts.
        wantCapture = true;
    } else if (!cache.healthKnown && healthProbes < 12 && cache.roundValue >= 1 &&
               cache.roundValue <= 5) {
        // Health cells are read only when the game draws a fighter's damage, and the pause menu draws
        // no damage: take a few short, rate-limited captures during the fight until one catches it.
        wantCapture = true;
        healthProbe = true;
    }

    const bool armedNow = armed.load();
    if (!armedNow && captureFresh.exchange(false)) {
        // A capture just closed: turn what it saw into cells (or keep the ones we have).
        const std::uintptr_t store = static_cast<std::uintptr_t>(statStore.load());
        if (store != 0 && learnedCount.load() > 0) {
            Rebuild();
        }
        wantCapture = false;
    } else if (!armedNow && wantCapture && now >= nextCaptureAt) {
        nextCaptureAt = now + 8.0;
        if (bellSeen) {
            // Only a new fight clears the cache; a health top-up must not disturb a running one.
            cache.valid = false;
            bellSeen = false;
        }
        if (healthProbe) {
            healthProbes += 1;
        }
        Arm();
    }

    if (!cache.valid) {
        return;
    }
    PollKnockdowns();
#endif
}

bool Ready() {
    return cache.valid;
}

const char* Status() {
    return status;
}

/// One stat id at a time: its strides, then the raw sub-slots of every slot and round cell. Printed
/// by the card key when dump_stats is on, so what the card reads can be compared with the game's own
/// statistics page *at the same instant* (press the key while that page is on screen).
void DumpArray(const char* name, int id, int dimensions) {
    const Array array = Lookup(cache.store, id, dimensions);
    if (!array.ok) {
        std::printf("Broadcast: dump %s (%d): not in the store\n", name, id);
        return;
    }
    std::int32_t strides[3] = {0, 0, 0};
    for (int i = 0; i < 3 && i < array.dimensions; ++i) {
        Read(array.strides + std::uintptr_t(i) * 4, strides[i]);
    }
    std::printf("Broadcast: dump %s (%d): n=%d strides=%d,%d,%d round=%d\n", name, id, array.count,
                int(strides[0]), int(strides[1]), int(strides[2]), cache.roundValue);
    for (int slot = 0; slot < 2; ++slot) {
        for (int round = 0; round <= cache.roundValue && round <= 5; ++round) {
            if (strides[1] > 0 && round * strides[1] >= array.count) {
                break;
            }
            const int base = slot * strides[0] + round * strides[1];
            char line[400];
            int used = std::snprintf(line, sizeof(line), "Broadcast:   %s slot %d round %d raw:", name,
                                     slot, round);
            for (int k = 0; k < 30; ++k) {
                const int at = base + k;
                if (at < 0 || at >= array.count || (strides[1] > 0 && k >= strides[1])) {
                    break;
                }
                std::int32_t raw = 0;
                if (!Read(array.values + std::uintptr_t(at) * 4, raw)) {
                    break;
                }
                used += std::snprintf(line + used, sizeof(line) - std::size_t(used), " %d", int(raw));
                if (used > int(sizeof(line)) - 8) {
                    break;
                }
            }
            std::printf("%s\n", line);
        }
    }
}

void DumpHealth() {
    if (!cache.healthKnown) {
        std::uintptr_t head = 0;
        std::uintptr_t overall = 0;
        const bool trappedHead = LearnedBase(IdHeadHealth, head);
        const bool trappedOverall = LearnedBase(IdOverallHealth, overall);
        const bool storedHead = Lookup(cache.store, IdHeadHealth, 1).ok;
        const bool storedOverall = Lookup(cache.store, IdOverallHealth, 1).ok;
        std::printf("Broadcast: dump health: unknown (learned head=%d overall=%d, store head=%d "
                    "overall=%d)\n",
                    trappedHead ? 1 : 0, trappedOverall ? 1 : 0, storedHead ? 1 : 0,
                    storedOverall ? 1 : 0);
        char ids[512];
        int used = 0;
        used += std::snprintf(ids + used, sizeof(ids) - std::size_t(used), "Broadcast:   learned ids:");
        const int total = std::min(learnedCount.load(), 96);
        for (int i = 0; i < total && used < int(sizeof(ids)) - 12; ++i) {
            used += std::snprintf(ids + used, sizeof(ids) - std::size_t(used), " %u",
                                  unsigned(learned[i].id));
        }
        std::printf("%s\n", ids);
        return;
    }
    for (int block = 0; block < 2; ++block) {
        const std::uintptr_t address = block == 0 ? cache.headHealthBlock : cache.overallHealthBlock;
        char line[256];
        int used = std::snprintf(line, sizeof(line), "Broadcast:   health %s:",
                                 block == 0 ? "head" : "overall");
        for (int i = 0; i < 4; ++i) {
            std::int32_t raw = 0;
            Read(address + std::uintptr_t(i) * 4, raw);
            float asFloat = 0.0f;
            std::memcpy(&asFloat, &raw, sizeof(asFloat));
            used += std::snprintf(line + used, sizeof(line) - std::size_t(used), " [%d]=%d/%.2f", i,
                                  int(raw), double(asFloat));
        }
        std::printf("%s\n", line);
    }
}

void Dump() {
    if (!cache.valid) {
        std::printf("Broadcast: dump: no capture yet\n");
        return;
    }
    Fighter home;
    Fighter away;
    ReadFighter(1, 0, true, home);
    ReadFighter(0, 0, false, away);
    std::printf("Broadcast: dump card: home sig=%d/%d total=%d td=%d/%d kd=%d head=%d overall=%d\n",
                home.sigLanded, home.sigThrown, home.totalLanded, home.takedownsLanded,
                home.takedownsAttempted, home.knockedDown, home.headHealth, home.overallHealth);
    std::printf("Broadcast: dump card: away sig=%d/%d total=%d td=%d/%d kd=%d head=%d overall=%d\n",
                away.sigLanded, away.sigThrown, away.totalLanded, away.takedownsLanded,
                away.takedownsAttempted, away.knockedDown, away.headHealth, away.overallHealth);
    DumpArray("total_landed", IdTotalStrikesLanded, 3);
    DumpArray("total_thrown", IdTotalStrikesThrown, 3);
    DumpArray("takedowns_landed", IdTakedownsLanded, 3);
    DumpArray("takedowns_attempted", IdTakedownsAttempted, 2);
    DumpArray("sig_landed", IdSigLanded, 2);
    DumpArray("sig_thrown", IdSigThrown, 2);
    DumpArray("knocked_down", IdKnockedDown, 2);
    DumpHealth();
}

bool InFight() {
    if (!cache.valid) {
        return false;
    }
    return cache.roundValue >= 1 && cache.roundValue <= 5 && cache.remainingValue >= 0 &&
           cache.remainingValue <= 900;
}

bool Paused() {
    if (!InFight()) {
        return true;
    }
    if (cache.betweenRoundsValue > 0) {
        return true;
    }
    // The clock not moving for a second means the game is not running the fight (pause menu, walk-in,
    // post-fight): the overlay never draws a card over those screens.
    return Now() - lastClockChangeAt > 1.0;
}

int Round() {
    return cache.valid ? cache.roundValue : 0;
}

bool PreviousRound(Snapshot& out) {
    if (!cache.valid || cache.roundValue < 2) {
        return false;
    }
    const int round = cache.roundValue - 1;
    ReadFighter(1, round, true, out.home);
    ReadFighter(0, round, false, out.away);
    return true;
}

bool FightSoFar(Snapshot& out) {
    if (!cache.valid) {
        return false;
    }
    Snapshot total;
    ReadFighter(1, 0, true, total.home);
    ReadFighter(0, 0, false, total.away);
    // The game keeps the fight total at round 0. When it has not been written (a store that only
    // fills per-round cells), add the rounds up instead.
    const bool totalEmpty = total.home.sigThrown == 0 && total.away.sigThrown == 0 &&
                            cache.roundValue >= 1;
    if (totalEmpty) {
        total = Snapshot{};
        total.home.headHealth = 100;
        total.away.headHealth = 100;
        total.home.overallHealth = 100;
        total.away.overallHealth = 100;
        for (int round = 1; round <= cache.roundValue && round <= 5; ++round) {
            Fighter home;
            Fighter away;
            ReadFighter(1, round, true, home);
            ReadFighter(0, round, false, away);
            total.home.sigLanded += home.sigLanded;
            total.home.sigThrown += home.sigThrown;
            total.home.totalLanded += home.totalLanded;
            total.home.takedownsLanded += home.takedownsLanded;
            total.home.takedownsAttempted += home.takedownsAttempted;
            total.home.knockedDown += home.knockedDown;
            total.away.sigLanded += away.sigLanded;
            total.away.sigThrown += away.sigThrown;
            total.away.totalLanded += away.totalLanded;
            total.away.takedownsLanded += away.takedownsLanded;
            total.away.takedownsAttempted += away.takedownsAttempted;
            total.away.knockedDown += away.knockedDown;
        }
    }
    // Health is live, never summed: the bars show how the fighters look right now.
    int value = 0;
    if (Health(cache.headHealthBlock, true, value)) {
        total.home.headHealth = value;
    }
    if (Health(cache.headHealthBlock, false, value)) {
        total.away.headHealth = value;
    }
    if (Health(cache.overallHealthBlock, true, value)) {
        total.home.overallHealth = value;
    }
    if (Health(cache.overallHealthBlock, false, value)) {
        total.away.overallHealth = value;
    }
    out = total;
    return true;
}

Knockdown PollKnockdown() {
    Knockdown result;
    // Consumed by the caller: the serial is what makes each knockdown a single event.
    result = lastKnockdown;
    return result;
}

} // namespace Stats
} // namespace Broadcast
