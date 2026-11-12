#include "model/model.hpp"
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace animus {
Issue make_issue(IssueKind kind, std::string code, std::string message) {
    return {kind, std::move(code), std::move(message), {}, {}, {}, {}, {}, {}};
}
Mask width_mask(unsigned width) {
    return width == 32 ? 0xFFFFFFFFu : (Word{1} << width) - 1;
}
Result<void> validate_config(const ModelConfig& config) {
    if (config.max_work_items == 0 || (config.raster_profile != RasterProfile::Disabled && config.raster_profile != RasterProfile::ReferenceV1))
        return make_issue(IssueKind::HostError, "InvalidConfig", "Invalid work budget or raster profile.");
    if (config.max_width == 0 || config.max_height == 0)
        return make_issue(IssueKind::HostError, "InvalidConfig", "Texture dimension limits must be nonzero.");
    if (config.warp_width < 1 || config.warp_width > kMaxLanes || config.max_warps != kWarpSlots)
        return make_issue(IssueKind::HostError, "InvalidConfig", "Require 1..32 lanes and exactly 4 warp slots.");
    if (config.memory_bytes == 0 || config.memory_bytes > (1ull << 32) ||
        config.memory_bytes > std::numeric_limits<std::size_t>::max() || config.max_steps == 0)
        return make_issue(IssueKind::HostError, "InvalidConfig", "Invalid memory size or instruction budget.");
    if ((config.fp_state_policy != FpStatePolicy::Unspecified && config.fp_state_policy != FpStatePolicy::PerLaneExperimental) ||
        (config.sfu_profile != SfuProfile::Disabled && config.sfu_profile != SfuProfile::HostMathV1))
        return make_issue(IssueKind::HostError, "InvalidConfig", "Unknown numeric policy/profile.");
    return {};
}
Result<Machine> create_machine(const ModelConfig& config) {
    auto valid = validate_config(config);
    if (!valid) return valid.error();
    try { return Machine(config); }
    catch (const std::bad_alloc&) { return make_issue(IssueKind::HostError, "AllocationFailed", "Cannot allocate model memory."); }
    catch (const std::length_error&) { return make_issue(IssueKind::HostError, "AllocationFailed", "Memory exceeds container capacity."); }
}
Result<std::optional<WarpId>> create_warp(Machine& machine, const LaunchInfo& launch) {
    if (machine.pending_spawn)
        return make_issue(IssueKind::HostError, "SpawnInProgress", "Do not inject host warps into a deferred WSPAWN.");
    if ((launch.init_mask & ~width_mask(machine.config.warp_width)) != 0)
        return make_issue(IssueKind::HostError, "InvalidMask", "Launch mask exceeds configured lane width.");
    if (launch.init_mask == 0) return std::optional<WarpId>{};
    auto dispatch_valid = validate_dispatch(machine, launch);
    if (!dispatch_valid) return dispatch_valid.error();
    auto entry = machine.memory.fetch32(launch.entry);
    if (!entry) return entry.error();
    auto code_regions = launch.code_regions;
    if (code_regions.empty()) {
        auto mapped = machine.memory.executable_region_at(launch.entry);
        if (!mapped) return mapped.error();
        code_regions.push_back(mapped.value());
    }
    bool contains_entry = false;
    for (const auto& region : code_regions) {
        if ((region.base & 3u) != 0 || (region.bytes & 3u) != 0)
            return make_issue(IssueKind::HostError, "InvalidCodeRegion", "Code regions must be word aligned.");
        auto checked = machine.memory.check_access(region.base, region.bytes, Access::Execute);
        if (!checked) return checked.error();
        if (launch.entry >= region.base && std::uint64_t{launch.entry} + 4 <= std::uint64_t{region.base} + region.bytes)
            contains_entry = true;
    }
    if (!contains_entry) return make_issue(IssueKind::HostError, "InvalidCodeRegion", "Entry is outside declared shader code.");
    for (WarpId id = 0; id < kWarpSlots; ++id) {
        if (machine.warps[id].allocated) continue;
        auto& warp = machine.warps[id];
        warp = WarpState{};
        warp.allocated = true;
        warp.invocation_id = machine.next_invocation_id++;
        warp.id = id;
        warp.pc = launch.entry;
        warp.init_mask = warp.exec = launch.init_mask;
        warp.code_regions = code_regions;
        warp.dispatch = launch.dispatch;
        return std::optional<WarpId>{id};
    }
    return make_issue(IssueKind::ModelLimit, "NoFreeWarpSlot", "All four warp slots are allocated.");
}
Result<void> initialize_x(Machine& machine, WarpId id, unsigned lane, unsigned reg, Word value) {
    if (id >= kWarpSlots || !machine.warps[id].allocated || lane >= machine.config.warp_width || reg >= kRegisters)
        return make_issue(IssueKind::HostError, "InvalidRegister", "Invalid initial register location.");
    auto& state = machine.warps[id].lanes[lane];
    if (reg != 0) { state.x[reg] = value; state.x_defined[reg] = true; }
    return {};
}
Result<void> initialize_f(Machine& machine, WarpId id, unsigned lane, unsigned reg, Word bits) {
    if (id >= kWarpSlots || !machine.warps[id].allocated || lane >= machine.config.warp_width || reg >= kRegisters)
        return make_issue(IssueKind::HostError, "InvalidRegister", "Invalid initial floating register location.");
    auto& state = machine.warps[id].lanes[lane];
    state.f_bits[reg] = bits; state.f_defined[reg] = true; // f0 is writable, unlike x0.
    return {};
}
Result<void> finish_test_warp(Machine& machine, WarpId id, Addr stop_pc) {
    if (machine.pending_spawn)
        return make_issue(IssueKind::HostError, "SpawnInProgress", "Use TMC-zero to finish peers while WSPAWN waits.");
    if (id >= kWarpSlots || !machine.warps[id].allocated)
        return make_issue(IssueKind::HostError, "InvalidWarp", "Warp slot is not allocated.");
    auto& warp = machine.warps[id];
    if (warp.dispatch.stage != ShaderStage::Test || warp.status != WarpStatus::Ready || warp.pc != stop_pc || warp.sp != 0 || warp.exec != warp.init_mask)
        return make_issue(IssueKind::HostError, "NotAtTestExit", "Host test completion requires Test stage and a reconverged warp at its stop PC.");
    warp.status = WarpStatus::Ended;
    return {};
}
Result<WarpState> release_warp(Machine& machine, WarpId id) {
    if (id >= kWarpSlots || !machine.warps[id].allocated)
        return make_issue(IssueKind::HostError, "InvalidWarp", "Warp slot is not allocated.");
    auto& warp = machine.warps[id];
    for (const auto& barrier : machine.barriers)
        if (barrier.arrived & (Mask{1} << id))
            return make_issue(IssueKind::HostError, "BarrierInProgress", "Cannot release a participant in an unfinished barrier round.");
    if (warp.status != WarpStatus::Ended && warp.status != WarpStatus::Faulted)
        return make_issue(IssueKind::HostError, "WarpStillLive", "Cannot release a live/paused/waiting warp.");
    auto snapshot = warp;
    warp = WarpState{};
    return snapshot;
}
std::optional<WarpId> choose_ready(const Machine& machine, std::optional<WarpId> after, Mask excluded) {
    const unsigned start = after ? (*after + 1) % kWarpSlots : 0;
    for (unsigned offset = 0; offset < kWarpSlots; ++offset) {
        const auto id = static_cast<WarpId>((start + offset) % kWarpSlots);
        const auto& warp = machine.warps[id];
        if (warp.allocated && warp.status == WarpStatus::Ready && (excluded & (Mask{1} << id)) == 0) return id;
    }
    return {};
}
} // namespace animus
