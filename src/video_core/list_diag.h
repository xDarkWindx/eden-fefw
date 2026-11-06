// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_LIST_DIAG: the diagnostic build of docs/list-diag-design.md. Off by default. This header
// is what the emulator's hook sites call; the protocol itself is video_core/list_diag/protocol.h,
// shared with tools/list-diag-harness. T3a: the parts `pop` and `blk`. The part `list` is T3b and
// is only recognised here.
//
// Every wrapper starts with the read of the flag and a branch. With the diagnostic off nothing else
// runs, nothing is allocated (the state is created the first time a part is on) and no counter moves.
#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/hardware_properties.h"
#include "video_core/list_diag/protocol.h"

namespace VideoCommon::ListDiag {

enum Part : u32 {
    PartPop = 1,  ///< Problem A: the pop window
    PartBlk = 2,  ///< Problem B: 64-byte blocks
    PartList = 4, ///< Command list entries (T3b, not implemented yet)
    PartBlkcmp = 8, ///< T3a.2: the old bytes of the GPU against the upload payload (needs blk; never in "1")
};

/// EDEN_LIST_DIAG: absent, empty or "0": nothing. "1": every part. Otherwise a list separated by
/// commas of `pop`, `blk` and `list`.
inline u32 ParseParts(const char* value) {
    if (value == nullptr || value[0] == '\0' || (value[0] == '0' && value[1] == '\0')) {
        return 0;
    }
    if (value[0] == '1' && value[1] == '\0') {
        return PartPop | PartBlk | PartList;
    }
    u32 parts = 0;
    const char* p = value;
    while (*p != '\0') {
        const char* end = std::strchr(p, ',');
        const size_t length = end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p);
        if (length == 3 && std::strncmp(p, "pop", 3) == 0) parts |= PartPop;
        if (length == 3 && std::strncmp(p, "blk", 3) == 0) parts |= PartBlk;
        if (length == 4 && std::strncmp(p, "list", 4) == 0) parts |= PartList;
        if (length == 6 && std::strncmp(p, "blkcmp", 6) == 0) parts |= PartBlkcmp;
        if (end == nullptr) break;
        p = end + 1;
    }
    return parts;
}

/// Read once at the start. Not changed during a session (only the tests change it, with the system stopped).
inline std::atomic<u32> g_parts{ParseParts(std::getenv("EDEN_LIST_DIAG"))};

[[nodiscard]] inline bool Enabled(u32 part) {
    return (g_parts.load(std::memory_order_relaxed) & part) != 0;
}
[[nodiscard]] inline bool NoticesEnabled() {
    return (g_parts.load(std::memory_order_relaxed) & (PartPop | PartBlk)) != 0;
}

/// EDEN_LIST_DIAG_SAMPLE (one page of every N, default 8, 1 = all) and EDEN_LIST_DIAG_SEED (decimal or
/// 0x..., default 1): read once, fixed for the session, printed in the "ligado" line and the summary
/// (docs/list-diag-design.md, 3.7.1). An invalid value falls back to the default and the line says so.
struct SampleConfig {
    u64 n = 8;
    u64 seed = 1;
    bool n_invalid = false;
    bool seed_invalid = false;
    std::string n_text;
    std::string seed_text;
};

inline SampleConfig ParseSampleConfig(const char* sample, const char* seed) {
    SampleConfig config;
    const auto parse = [](const char* text, u64& out) {
        if (text == nullptr || text[0] == '\0') {
            return false;
        }
        char* end = nullptr;
        const unsigned long long value = std::strtoull(text, &end, 0);
        if (end == text || *end != '\0' || text[0] == '-') {
            return false;
        }
        out = value;
        return true;
    };
    if (sample != nullptr) {
        config.n_text = sample;
        u64 n = 0;
        if (parse(sample, n) && n >= 1 && n <= (u64{1} << 32)) {
            config.n = n;
        } else {
            config.n_invalid = true;
        }
    }
    if (seed != nullptr) {
        config.seed_text = seed;
        u64 value = 0;
        if (parse(seed, value)) {
            config.seed = value;
        } else {
            config.seed_invalid = true;
        }
    }
    return config;
}

inline const SampleConfig g_sample_config =
    ParseSampleConfig(std::getenv("EDEN_LIST_DIAG_SAMPLE"), std::getenv("EDEN_LIST_DIAG_SEED"));

using Impl = Protocol<Core::Hardware::NUM_CPU_CORES, ProdHooks>;

/// blkcmp (docs/list-diag-design.md, 3.8.5): EDEN_LIST_DIAG_BLKCMP_SEED (default derived from the sample seed),
/// _ODDS (1/M, default 4), _BUDGET (sustained comparisons per second, default 5), _BURST (bucket capacity,
/// default 5), _MAXBYTES (cap of at_risk bytes per range, default 4096). Read once; an invalid value falls back
/// to the default and the "ligado" line says so.
struct BlkcmpConfig {
    u64 seed = 0;
    u64 odds = 4;
    u64 rate = 5;
    u64 burst = 5;
    u64 max_bytes = 4096;
    std::string invalid; ///< names of the variables whose value was invalid
};

inline BlkcmpConfig ParseBlkcmpConfig(u64 sample_seed, const char* seed, const char* odds, const char* rate,
                                      const char* burst, const char* max_bytes) {
    BlkcmpConfig config;
    config.seed = Impl::SplitMix64(sample_seed ^ 0x626c6b636d70ull); // "blkcmp": uncorrelated with the page predicate
    const auto parse = [&](const char* text, const char* name, u64 lo, u64 hi, u64& out) {
        if (text == nullptr) {
            return;
        }
        char* end = nullptr;
        const unsigned long long value = text[0] != '\0' && text[0] != '-' ? std::strtoull(text, &end, 0) : 0;
        if (text[0] == '\0' || text[0] == '-' || *end != '\0' || value < lo || value > hi) {
            config.invalid += std::string{config.invalid.empty() ? "" : ","} + name;
            return;
        }
        out = value;
    };
    parse(seed, "SEED", 0, ~u64{0}, config.seed);
    parse(odds, "ODDS", 1, u64{1} << 32, config.odds);
    parse(rate, "BUDGET", 1, 1'000'000, config.rate);
    parse(burst, "BURST", 1, 1'000'000, config.burst);
    parse(max_bytes, "MAXBYTES", 64, u64{1} << 24, config.max_bytes);
    return config;
}

inline const BlkcmpConfig g_blkcmp_config = ParseBlkcmpConfig(
    g_sample_config.seed, std::getenv("EDEN_LIST_DIAG_BLKCMP_SEED"), std::getenv("EDEN_LIST_DIAG_BLKCMP_ODDS"),
    std::getenv("EDEN_LIST_DIAG_BLKCMP_BUDGET"), std::getenv("EDEN_LIST_DIAG_BLKCMP_BURST"),
    std::getenv("EDEN_LIST_DIAG_BLKCMP_MAXBYTES"));

/// Where the log lines go: the emulator's log (list_diag.cpp), or the standard error without it.
using LogSink = void (*)(const std::string&);
inline std::atomic<LogSink> g_sink{nullptr};
void InstallLogSink(); ///< list_diag.cpp (the tests of the harness define their own)

inline void Emit(const std::string& line) {
    if (const LogSink sink = g_sink.load(std::memory_order_acquire)) {
        sink(line);
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

inline std::atomic<Impl*> g_state{nullptr};

/// Allocated the first time a part is on, never otherwise.
inline Impl& State() {
    Impl* state = g_state.load(std::memory_order_acquire);
    if (state != nullptr) {
        return *state;
    }
    static Impl* const created = [] {
        InstallLogSink();
        auto* impl = new Impl();
        const u32 parts = g_parts.load(std::memory_order_relaxed);
        impl->pop_on = (parts & PartPop) != 0;
        impl->blk_on = (parts & PartBlk) != 0;
        impl->blkcmp_on = (parts & PartBlkcmp) != 0 && (parts & PartBlk) != 0;
        impl->blkcmp_seed = g_blkcmp_config.seed;
        impl->blkcmp_odds = g_blkcmp_config.odds;
        impl->blkcmp_rate = g_blkcmp_config.rate;
        impl->blkcmp_burst = g_blkcmp_config.burst;
        impl->blkcmp_max_bytes = g_blkcmp_config.max_bytes;
        impl->sample_n = g_sample_config.n;
        impl->sample_seed = g_sample_config.seed;
        const char* const merge = std::getenv("EDEN_HELD_WRITE_MERGE"); // as gpu.cpp reads it
        impl->merge_on = merge == nullptr || merge[0] != '0';
        g_state.store(impl, std::memory_order_release);
        return impl;
    }();
    return *created;
}

// ---------------------------------------------------------------------------------------------
// Hook sites.

/// core/memory.cpp, where the guest CPU holds a write: the Collect, observed when on.
template <class Manager>
inline void CollectNotice(Manager& manager, unsigned core, u64 address, size_t size) {
    if (!NoticesEnabled()) {
        manager.Collect(address, size);
        return;
    }
    State().CollectObserved(manager, core, address, static_cast<u32>(size));
}

/// buffer_cache.h, top of CachedWriteMemory.
template <class RangeSet, class ReadGuest>
inline void NoteApplyHook(u64 addr, u64 size, const RangeSet& gpu_modified, const RangeSet& cpu_read,
                          bool track_cpu_reads, ReadGuest&& read_guest) {
    if (!NoticesEnabled()) {
        return;
    }
    State().NoteApply(addr, size, gpu_modified, cpu_read, track_cpu_reads, read_guest);
}

/// buffer_cache.h, inside the loop of PopAsyncBuffers, before the bytes are written.
inline void NotePopHook(u64 start, u64 end, const u8* bytes) {
    if (!Enabled(PartPop)) {
        return;
    }
    State().NotePop(start, end, bytes);
}

// blkcmp hook sites. Off (the part not selected, or blk missing): the flag and nothing else.
[[nodiscard]] inline bool BlkcmpEnabled() {
    const u32 parts = g_parts.load(std::memory_order_relaxed);
    return (parts & PartBlkcmp) != 0 && (parts & PartBlk) != 0;
}

/// buffer_cache.h, first line of InlineMemoryImplementation: the request of this thread, if it is for this range.
[[nodiscard]] inline const Impl::BlkcmpRequest* BlkcmpPending(u64 addr, u64 size) {
    return BlkcmpEnabled() ? State().BlkcmpPending(addr, size) : nullptr;
}
inline void BlkcmpDiscard(BlkcmpReason reason) {
    if (BlkcmpEnabled()) {
        State().BlkcmpDiscard(reason);
    }
}
[[nodiscard]] inline bool BlkcmpTakeToken() {
    return State().BlkcmpTakeToken();
}
/// The comparison (3.8.2, steps 7): ends the request.
inline void BlkcmpCompare(const u8* old_bytes, const std::array<u64, kBlkcmpParts>& offsets, std::span<const u8> payload,
                          u64 finish_us) {
    State().BlkcmpCompare(old_bytes, offsets, payload, finish_us);
}

/// buffer_cache.h, CachedWriteMemory, right after the NoteApplyHook: whatever request is still pending when the
/// function leaves never reached the upload ("sem upload").
struct BlkcmpScope {
    BlkcmpScope() = default;
    BlkcmpScope(const BlkcmpScope&) = delete;
    BlkcmpScope& operator=(const BlkcmpScope&) = delete;
    ~BlkcmpScope() {
        if (BlkcmpEnabled()) {
            State().BlkcmpExpire();
        }
    }
};

inline std::string PartsText() {
    const u32 parts = g_parts.load(std::memory_order_relaxed);
    std::string text;
    for (const auto& [bit, name] :
         {std::pair<u32, const char*>{PartPop, "pop"}, {PartBlk, "blk"}, {PartList, "list"}, {PartBlkcmp, "blkcmp"}}) {
        if ((parts & bit) != 0) {
            text += text.empty() ? name : std::string{","} + name;
        }
    }
    return text;
}

/// gpu.cpp, when the GPU starts: ONE line that says the diagnostic is on and which parts (nothing
/// at all when it is off). Without it a log with no [ListDiag] line cannot tell "off" from "broken".
inline void LogStartup() {
    if (g_parts.load(std::memory_order_relaxed) == 0) {
        return;
    }
    State(); // creates the state and installs the log sink
    char sample[256];
    std::snprintf(sample, sizeof(sample), "; amostra=1/%llu de paginas de 4 KiB (semente=0x%llx, hash=splitmix64)%s%s",
                  static_cast<unsigned long long>(g_sample_config.n),
                  static_cast<unsigned long long>(g_sample_config.seed),
                  g_sample_config.n_invalid ? (" [EDEN_LIST_DIAG_SAMPLE='" + g_sample_config.n_text + "' invalido: padrao 8]").c_str() : "",
                  g_sample_config.seed_invalid ? (" [EDEN_LIST_DIAG_SEED='" + g_sample_config.seed_text + "' invalido: padrao 1]").c_str() : "");
    std::string blkcmp;
    if (Enabled(PartBlkcmp)) {
        if (!Enabled(PartBlk)) {
            blkcmp = "; blkcmp IGNORADO: exige blk";
        } else {
            blkcmp = "; blkcmp: sorteio 1/" + std::to_string(g_blkcmp_config.odds) + " das faixas afirmadas com at_risk>0 (semente=0x" +
                     [&] {
                         char b[24];
                         std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(g_blkcmp_config.seed));
                         return std::string{b};
                     }() +
                     "), balde " + std::to_string(g_blkcmp_config.rate) + "/s sustentado e rajada de " +
                     std::to_string(g_blkcmp_config.burst) + ", teto " + std::to_string(g_blkcmp_config.max_bytes) +
                     " B at_risk por faixa; versao sincrona: um Finish por comparacao, sob o buffer_cache.mutex" +
                     (g_blkcmp_config.invalid.empty() ? "" : " [invalido, padrao usado: " + g_blkcmp_config.invalid + "]");
        }
    }
    Emit("[ListDiag] ligado: partes=" + PartsText() + (Enabled(PartList) ? " (list: T3b, ainda nao implementada)" : "") +
         sample + blkcmp + "; resumo ao fechar o jogo e totais a cada 30 s (aproximados: lidos com a execucao em andamento)");
}

/// gpu.cpp, after an application: the log, at most once per second, outside every cache lock; and
/// every 30 seconds a line of totals, so that the log holds data even if the game is killed
/// instead of closed from the menu (the summary is written at shutdown only).
inline void DrainLog() {
    if (g_state.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    Impl& state = State();
    state.MaybeDrain(Emit);
    static std::atomic<u64> last_heartbeat{0};
    const u64 now = ProdHooks::NowUs();
    u64 last = last_heartbeat.load(std::memory_order_relaxed);
    if (last == 0) {
        last_heartbeat.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last >= 30'000'000 && last_heartbeat.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        for (const std::string& line : state.SummaryLines("total")) {
            Emit(line);
        }
    }
}

/// back_thread_stats.cpp Report(): the lines of the periodic block, if the diagnostic is on.
inline std::vector<std::string> ReportLines(double elapsed_seconds) {
    if (g_state.load(std::memory_order_acquire) == nullptr) {
        return {};
    }
    return State().ReportLines(elapsed_seconds);
}

/// At the end of the session: the summary (repeats the P0 notice if it applies).
inline void LogSummary() {
    if (g_state.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    Impl& state = State();
    state.Drain(Emit);
    for (const std::string& line : state.SummaryLines()) {
        Emit(line);
    }
}

} // namespace VideoCommon::ListDiag
