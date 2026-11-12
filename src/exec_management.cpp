#include "model/execution.hpp"
#include <algorithm>
#include <bit>
#include <utility>

namespace animus {
namespace {
Issue fault(const char* code, const char* message) {
    return make_issue(IssueKind::ArchitecturalFault, code, message);
}
bool live(const WarpState& warp) {
    return warp.allocated && (warp.status == WarpStatus::Ready || warp.status == WarpStatus::Waiting);
}
bool graphics_in_flight(const Machine& machine) {
    if (machine.submission.status == SubmissionStatus::Busy) return true;
    for (const auto& warp : machine.warps)
        if (warp.allocated && warp.dispatch.stage != ShaderStage::Test) return true;
    return false;
}
// Vortex defers WSPAWN until the calling warp is the only active warp.
// No clocks are needed: a TMC-zero retirement can make the request ready.
void complete_spawn(ManagementEffect& update, unsigned width) {
    if (!update.pending_spawn) return;
    const auto request = *update.pending_spawn;
    for (const auto& warp : update.warps)
        if (warp.id != request.caller && live(warp)) return;
    const auto regions = update.warps[request.caller].code_regions;
    for (WarpId id = 0; id < request.count; ++id) {
        if (id == request.caller) continue;
        WarpState child;
        child.allocated = true; child.id = id; child.pc = request.entry;
        // Management has no dispatched shader invocation. INIT_MASK is its
        // allowed full-lane envelope; TMC may expand the initial lane-0 EXEC.
        child.init_mask = width_mask(width); child.exec = 1;
        child.invocation_id = update.next_invocation_id++;
        child.code_regions = regions;
        // Registers deliberately start undefined (except x0); never clone the
        // parent's stack/arguments or a previous occupant's values.
        update.warps[id] = std::move(child);
    }
    auto& caller = update.warps[request.caller];
    caller.status = WarpStatus::Ready; caller.wait_reason = WaitReason::None;
    update.pending_spawn.reset();
}
}

Result<StepEffect> eval_management(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    if (warp.sp != 0) return fault("ManagementInsideSplit", "Warp control requires an empty SIMT stack.");
    if (inst.op != Op::Barrier && (warp.dispatch.stage != ShaderStage::Test || graphics_in_flight(machine)))
        return fault("ManagementDuringShader", "TMC/WSPAWN require management code with no in-flight VS/FS contexts.");
    // Scalar warp-control arguments come from the highest active lane, as in
    // the referenced Vortex simx. Inactive/unselected lanes are not read.
    const unsigned lane = std::bit_width(warp.exec) - 1;
    const Word arg1 = read_x(warp, lane, inst.rs1);
    const Word arg2 = inst.uses_rs2 ? read_x(warp, lane, inst.rs2) : 0;
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    ManagementEffect update;
    update.warps.assign(machine.warps.begin(), machine.warps.end());
    update.barriers = machine.barriers;
    update.pending_spawn = machine.pending_spawn;
    update.next_invocation_id = machine.next_invocation_id;
    auto& current = update.warps[id];
    current.pc = effect.next_pc;
    if (inst.op == Op::ThreadMaskControl) {
        const Mask next = arg1 & width_mask(machine.config.warp_width);
        if (next & ~warp.init_mask)
            return fault("MaskOutsideLaunch", "TMC cannot enable lanes outside the host management envelope.");
        current.exec = next;
        if (next == 0) { current.status = WarpStatus::Ended; current.wait_reason = WaitReason::None; }
        complete_spawn(update, machine.config.warp_width);
    } else if (inst.op == Op::WarpSpawn) {
        if (machine.pending_spawn)
            return make_issue(IssueKind::ModelLimit, "SpawnPending", "A deferred WSPAWN is already waiting; run its peers first.");
        const Word count = std::min(arg1, Word{kWarpSlots});
        // Validate all reusable slots before accepting any request. Live Test
        // peers can finish while the request waits; faulted/graphics snapshots
        // must be explicitly collected/released by the host instead.
        for (WarpId target = 0; target < count; ++target) {
            if (target == id) continue;
            const auto& slot = machine.warps[target];
            if (slot.allocated && (slot.dispatch.stage != ShaderStage::Test || slot.status == WarpStatus::Faulted || slot.completed_outputs))
                return make_issue(IssueKind::ModelLimit, "SpawnSlotInUse", "Target slot contains a snapshot that the host must release.");
        }
        if (count > 0 && !(count == 1 && id == 0)) {
            auto aligned = check_target_alignment(arg2);
            if (!aligned) return aligned.error();
            // Target permissions are checked when children actually fetch,
            // consistently with ordinary jumps; don't decode their first word.
        }
        update.pending_spawn = PendingSpawn{id, count, arg2};
        current.status = WarpStatus::Waiting; current.wait_reason = WaitReason::Spawn;
        complete_spawn(update, machine.config.warp_width);
    } else if (inst.op == Op::Barrier) {
        if (warp.exec != warp.init_mask)
            return fault("BarrierPartialMask", "BAR requires EXEC == INIT_MASK and an empty SIMT stack.");
        if (arg1 & 0x80000000u)
            return make_issue(IssueKind::ModelLimit, "GlobalBarrierUnsupported", "Only single-core local barriers are modeled; rs1 bit 31 requests a global barrier.");
        // Vortex packed identifier: group in low byte, barrier number above it.
        const Word group = arg1 & 0xFFu, number = arg1 >> 8;
        if (group >= kWarpSlots || number >= kBarriersPerGroup)
            return make_issue(IssueKind::ModelLimit, "BarrierCapacity", "Local group/barrier identifier exceeds the declared host capacity.");
        if (arg2 == 0 || arg2 > kWarpSlots)
            return fault("InvalidBarrierCount", "Local BAR count is the number of arriving warps, in 1..4, not lanes or count-minus-one.");
        auto& barrier = update.barriers[group * kBarriersPerGroup + number];
        if (barrier.arrived && barrier.expected != arg2)
            return fault("BarrierCountMismatch", "All arrivals in one barrier round must specify the same warp count.");
        if (barrier.arrived & (Mask{1} << id))
            return fault("DuplicateBarrierArrival", "A warp can contribute only once to a blocking barrier round.");
        barrier.expected = arg2;
        barrier.arrived |= Mask{1} << id;
        current.status = WarpStatus::Waiting; current.wait_reason = WaitReason::Barrier;
        if (std::popcount(barrier.arrived) == static_cast<int>(arg2)) {
            // All previous stores already committed to the shared byte array.
            // Release every participant at PC+4; never execute BAR a second time.
            for (WarpId participant = 0; participant < kWarpSlots; ++participant) {
                if (!(barrier.arrived & (Mask{1} << participant))) continue;
                auto& waiting = update.warps[participant];
                waiting.status = WarpStatus::Ready; waiting.wait_reason = WaitReason::None;
            }
            barrier.arrived = 0; barrier.expected = 0; ++barrier.generation;
        }
    } else return make_issue(IssueKind::HostError, "InvalidDecodedOp", "Unknown warp-management operation.");
    effect.control = ControlUpdate{current.exec, current.sp, current.stack};
    effect.next_pc = current.pc; // A deferred launch may immediately reuse this retiring slot.
    effect.next_status = current.status;
    effect.management = std::move(update);
    return effect;
}
} // namespace animus
