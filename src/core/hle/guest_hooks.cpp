// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hardware_properties.h"
#include "core/hle/guest_hooks.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"

namespace Core::HLE::GuestHooks {

namespace {

constexpr u32 EntrySvcBase = 0x7C00;
constexpr u32 ReturnSvc = 0x7C7F;
// Stub words reserved at the end of a text segment: the return stub plus 8 bytes per entry stub.
constexpr u64 StubBytes = 0x40;
constexpr size_t MaxHooks = 7;

struct Spec {
    std::string module;
    u64 offset;
    std::vector<std::pair<u32, u64>> sets; // registers overridden on entry ("=x1:1")
    bool indirect;                         // "<module>*<offset>": hook the function stored there
};

struct Hook {
    std::string name;
    u64 address;     // patched instruction
    u64 stub;        // relocated instruction + branch back (entry hooks only)
    u64 return_stub; // address of the return svc of the module
    u32 call_reg;    // register of the replaced `blr`, or ~0u for an entry hook
    u32 adrp;        // replaced leading `adrp`, emulated in the handler (0 if none)
    std::vector<std::pair<u32, u64>> sets;
};

// Indirect hook waiting for its pointer slot to be filled at run time.
struct Pending {
    std::string label;
    u64 slot;
    u64 text_begin;
    u64 text_end;
    u64 stub;
    u64 return_stub;
    std::vector<std::pair<u32, u64>> sets;
};

struct ModuleRange {
    std::string name;
    u64 base;
    u64 size;
};

std::mutex mutex;
std::vector<Hook> hooks;
std::vector<ModuleRange> modules;
std::unordered_map<u64, std::vector<u64>> saved_returns; // keyed by sp at the hooked instruction
std::atomic<u32> lines{0};
std::vector<Pending> pending;
std::atomic<bool> has_pending{false};

/// EDEN_GUEST_HOOKS_LINES raises the log cap (default 20000) for long runs.
u32 LineLimit() {
    static const u32 limit = [] {
        const char* env = std::getenv("EDEN_GUEST_HOOKS_LINES");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 0)) : 20000u;
    }();
    return limit;
}

const std::vector<Spec>& Specs() {
    static const std::vector<Spec> specs = [] {
        std::vector<Spec> result;
        const char* env = std::getenv("EDEN_GUEST_HOOKS");
        if (env == nullptr) {
            return result;
        }
        std::string_view text{env};
        while (!text.empty()) {
            const size_t comma = text.find(',');
            const std::string_view item = text.substr(0, comma);
            text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            std::string_view location = item;
            std::vector<std::pair<u32, u64>> sets;
            if (const size_t equals = item.find('='); equals != std::string_view::npos) {
                location = item.substr(0, equals);
                std::string_view rest = item.substr(equals + 1);
                while (!rest.empty()) {
                    const size_t semi = rest.find(';');
                    const std::string set{rest.substr(0, semi)};
                    rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
                    char* reg_end = nullptr;
                    const unsigned long reg =
                        set.size() > 1 && set[0] == 'x' ? std::strtoul(set.c_str() + 1, &reg_end, 10) : 99;
                    if (reg > 28 || reg_end == nullptr || *reg_end != ':') {
                        LOG_ERROR(Core, "GuestHooks: ignoring set '{}', expected xN:value", set);
                        continue;
                    }
                    sets.emplace_back(static_cast<u32>(reg), std::strtoull(reg_end + 1, nullptr, 0));
                }
            }
            const size_t plus = location.find_first_of("+*");
            if (plus == std::string_view::npos || plus == 0) {
                LOG_ERROR(Core, "GuestHooks: ignoring '{}', expected <module>+<offset> or <module>*<offset>",
                          item);
                continue;
            }
            const std::string offset{location.substr(plus + 1)};
            char* end = nullptr;
            const u64 value = std::strtoull(offset.c_str(), &end, 0);
            if (end == offset.c_str() || *end != '\0') {
                LOG_ERROR(Core, "GuestHooks: ignoring '{}', bad offset", item);
                continue;
            }
            result.push_back(Spec{std::string{location.substr(0, plus)}, value, std::move(sets),
                                  location[plus] == '*'});
        }
        return result;
    }();
    return specs;
}

bool PcRelative(u32 insn) {
    return (insn & 0x7c000000) == 0x14000000 ||  // b / bl
           (insn & 0x7e000000) == 0x34000000 ||  // cbz / cbnz
           (insn & 0x7e000000) == 0x36000000 ||  // tbz / tbnz
           (insn & 0xff000010) == 0x54000000 ||  // b.cond
           (insn & 0x1f000000) == 0x10000000 ||  // adr / adrp
           (insn & 0x3b000000) == 0x18000000;    // ldr literal
}

bool IsAdrp(u32 insn) {
    return (insn & 0x9f000000) == 0x90000000;
}

bool IsBlr(u32 insn) {
    return (insn & 0xfffffc1f) == 0xd63f0000;
}

std::string Describe(u64 address) {
    for (const auto& module : modules) {
        if (address >= module.base && address < module.base + module.size) {
            return fmt::format("{}+{:#x}", module.name, address - module.base);
        }
    }
    return "?";
}

u64 Register(const Kernel::Svc::ThreadContext& ctx, u32 index) {
    if (index < ctx.r.size()) {
        return ctx.r[index];
    }
    return index == 29 ? ctx.fp : index == 30 ? ctx.lr : ctx.sp;
}

} // namespace

void InstallModule(Kernel::KProcess& process, std::string_view name, u64 base, u64 size,
                   u64 text_size) {
    if (std::getenv("EDEN_HOST_SAMPLER")) {
        // Lets host-sampler JIT samples be resolved to <module>+<offset>.
        LOG_ERROR(Core, "GuestHooks: module {} base={:#x} size={:#x}", name, base, size);
    }
    const auto& specs = Specs();
    if (specs.empty()) {
        return;
    }
    std::scoped_lock lock{mutex};
    modules.push_back(ModuleRange{std::string{name}, base, size});

    const u64 page_end = Common::AlignUp(text_size, 0x1000);
    auto& memory = process.GetMemory();
    u64 stub_base = page_end - StubBytes;
    if (text_size > stub_base) {
        // The text segment has no zero padding to hold the stubs.
        stub_base = 0;
    }
    u64 used = 0;
    for (const Spec& spec : specs) {
        if (spec.module != name) {
            continue;
        }
        const std::string label = fmt::format("{}+{:#x}", name, spec.offset);
        if (stub_base == 0 || used >= MaxHooks || (!spec.indirect && spec.offset >= text_size)) {
            LOG_ERROR(Core, "GuestHooks: {} skipped (no stub space or outside the text segment)", label);
            continue;
        }
        const u64 return_stub = base + stub_base;
        if (used == 0) {
            memory.Write32(return_stub, 0xd4000001 | (ReturnSvc << 5));
        }
        if (spec.indirect) {
            pending.push_back(Pending{label, base + spec.offset, base, base + text_size,
                                      base + stub_base + 8 + used * 8, return_stub, spec.sets});
            has_pending = true;
            ++used;
            LOG_ERROR(Core, "GuestHooks: {} pending until its pointer slot is filled", label);
            continue;
        }
        const u64 address = base + spec.offset;
        const u32 original = memory.Read32(address);
        const u32 index = static_cast<u32>(hooks.size());
        Hook hook{label, address, 0, return_stub, ~0u, 0, spec.sets};
        if (IsBlr(original)) {
            hook.call_reg = (original >> 5) & 31;
        } else if (IsAdrp(original)) {
            // NVN API trampolines start with adrp: emulate it rather than relocating it.
            hook.adrp = original;
        } else if (PcRelative(original)) {
            LOG_ERROR(Core, "GuestHooks: {} has a PC-relative instruction, skipped", label);
            continue;
        } else {
            hook.stub = base + stub_base + 8 + used * 8;
            const s64 distance = (static_cast<s64>(address + 4) - static_cast<s64>(hook.stub + 4)) / 4;
            memory.Write32(hook.stub, original);
            memory.Write32(hook.stub + 4, 0x14000000 | (static_cast<u32>(distance) & 0x03ffffff));
        }
        memory.Write32(address, 0xd4000001 | ((EntrySvcBase + index) << 5));
        hooks.push_back(std::move(hook));
        ++used;
        LOG_ERROR(Core, "GuestHooks: {} at {:#x} {}", label, address,
                  hooks.back().call_reg == ~0u ? "entry hook" : "call-site hook");
    }
}

namespace {

// Installs the indirect hooks whose pointer slot now holds a function in the module's text.
void ResolvePending(Core::System& system) {
    auto& kernel = system.Kernel();
    auto& process = Kernel::GetCurrentProcess(kernel);
    auto& memory = process.GetMemory();
    std::scoped_lock lock{mutex};
    for (auto it = pending.begin(); it != pending.end();) {
        const u64 target = memory.Read64(it->slot);
        if (target == 0) {
            ++it;
            continue;
        }
        const s64 reach = static_cast<s64>(target) - static_cast<s64>(it->stub);
        if (Describe(target) == "?" || reach >= (s64{1} << 27) || reach < -(s64{1} << 27)) {
            LOG_ERROR(Core, "GuestHooks: {} points to {:#x} ({}), not hookable from the stub, dropped",
                      it->label, target, Describe(target));
            it = pending.erase(it);
            continue;
        }
        if (std::ranges::any_of(hooks, [target](const Hook& h) { return h.address == target; })) {
            LOG_ERROR(Core, "GuestHooks: {} points to {} which is already hooked, skipped", it->label,
                      Describe(target));
            it = pending.erase(it);
            continue;
        }
        const u32 original = memory.Read32(target);
        const u32 index = static_cast<u32>(hooks.size());
        Hook hook{fmt::format("{}->{}", it->label, Describe(target)), target, 0,
                  it->return_stub, ~0u, 0, it->sets};
        if (IsAdrp(original)) {
            hook.adrp = original;
        } else if (PcRelative(original) || IsBlr(original)) {
            LOG_ERROR(Core, "GuestHooks: {} starts with an unsupported instruction {:#x}, dropped",
                      hook.name, original);
            it = pending.erase(it);
            continue;
        } else {
            hook.stub = it->stub;
            const s64 distance = (static_cast<s64>(target + 4) - static_cast<s64>(hook.stub + 4)) / 4;
            memory.Write32(hook.stub, original);
            memory.Write32(hook.stub + 4, 0x14000000 | (static_cast<u32>(distance) & 0x03ffffff));
        }
        memory.Write32(target, 0xd4000001 | ((EntrySvcBase + index) << 5));
        for (u32 core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core) {
            if (auto* const arm = process.GetArmInterface(core)) {
                arm->InvalidateCacheRange(target, 4);
                arm->InvalidateCacheRange(it->stub, 8);
            }
        }
        LOG_ERROR(Core, "GuestHooks: {} resolved, entry hook at {:#x}", hook.name, target);
        hooks.push_back(std::move(hook));
        it = pending.erase(it);
    }
    has_pending = !pending.empty();
}

} // namespace

bool HandleSvc(Core::System& system, u32 imm, std::span<u64, 8> args) {
    if (has_pending.load(std::memory_order_relaxed)) {
        ResolvePending(system);
    }
    if (imm < EntrySvcBase || imm > ReturnSvc) {
        return false;
    }
    auto& kernel = system.Kernel();
    auto& process = Kernel::GetCurrentProcess(kernel);
    auto* const arm = process.GetArmInterface(kernel.CurrentPhysicalCore().CoreIndex());
    Kernel::Svc::ThreadContext ctx{};
    arm->GetContext(ctx);
    std::scoped_lock lock{mutex};

    if (imm == ReturnSvc) {
        auto& stack = saved_returns[ctx.sp];
        if (stack.empty()) {
            LOG_ERROR(Core, "GuestHooks: return svc without a saved address, sp={:#x}", ctx.sp);
            return true;
        }
        const u64 resume = stack.back();
        stack.pop_back();
        if (lines.fetch_add(1) < LineLimit()) {
            LOG_ERROR(Core, "GuestHooks RET x0={:#x} -> {}", args[0], Describe(resume));
        }
        ctx.pc = resume;
        arm->SetContext(ctx);
        return true;
    }

    const u32 index = imm - EntrySvcBase;
    if (index >= hooks.size()) {
        return true;
    }
    const Hook& hook = hooks[index];
    u64 target = 0;
    if (hook.call_reg != ~0u) {
        target = Register(ctx, hook.call_reg);
        if (lines.fetch_add(1) < LineLimit()) {
            LOG_ERROR(Core,
                      "GuestHooks CALL {} target={:#x} ({}) x0={:#x} x1={:#x} x2={:#x} x3={:#x}",
                      hook.name, target, Describe(target), args[0], args[1], args[2], args[3]);
        }
        saved_returns[ctx.sp].push_back(hook.address + 4);
        ctx.lr = hook.return_stub;
        ctx.pc = target;
    } else if (hook.adrp != 0) {
        if (lines.fetch_add(1) < LineLimit()) {
            LOG_ERROR(Core, "GuestHooks ENTER {} x0={:#x} x1={:#x} x2={:#x} x3={:#x} lr={:#x} ({})",
                      hook.name, args[0], args[1], args[2], args[3], ctx.lr, Describe(ctx.lr));
        }
        const u64 immlo = (hook.adrp >> 29) & 3;
        const u64 immhi = (hook.adrp >> 5) & 0x7ffff;
        const s64 page_delta = static_cast<s64>(((immhi << 2) | immlo) << 43) >> 31;
        for (const auto& [reg, value] : hook.sets) {
            ctx.r[reg] = value;
        }
        ctx.r[hook.adrp & 31] = (hook.address & ~0xfffULL) + page_delta;
        ctx.pc = hook.address + 4;
    } else {
        for (const auto& [reg, value] : hook.sets) {
            ctx.r[reg] = value;
        }
        if (lines.fetch_add(1) < LineLimit()) {
            LOG_ERROR(Core, "GuestHooks ENTER {} x0={:#x} x1={:#x} x2={:#x} x3={:#x} lr={:#x} ({})",
                      hook.name, args[0], args[1], args[2], args[3], ctx.lr, Describe(ctx.lr));
        }
        saved_returns[ctx.sp].push_back(ctx.lr);
        ctx.lr = hook.return_stub;
        ctx.pc = hook.stub;
    }
    arm->SetContext(ctx);
    return true;
}

} // namespace Core::HLE::GuestHooks
