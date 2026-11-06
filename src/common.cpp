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
    if (config.warp_width < 1 || config.warp_width > kMaxLanes || config.max_warps != kWarpSlots)
        return make_issue(IssueKind::HostError, "InvalidConfig", "Require 1..32 lanes and exactly 4 warp slots.");
    if (config.memory_bytes == 0 || config.memory_bytes > (1ull << 32) ||
        config.memory_bytes > std::numeric_limits<std::size_t>::max() || config.max_steps == 0)
        return make_issue(IssueKind::HostError, "InvalidConfig", "Invalid memory size or instruction budget.");
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
    if ((launch.init_mask & ~width_mask(machine.config.warp_width)) != 0)
        return make_issue(IssueKind::HostError, "InvalidMask", "Launch mask exceeds configured lane width.");
    if (launch.init_mask == 0) return std::optional<WarpId>{};
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
        warp.id = id;
        warp.pc = launch.entry;
        warp.init_mask = warp.exec = launch.init_mask;
        warp.code_regions = code_regions;
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
} // namespace animus
