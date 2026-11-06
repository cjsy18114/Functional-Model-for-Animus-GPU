#include "model/model.hpp"
#include <optional>

namespace animus {
namespace {
Result<void> validate_warp(const Machine& machine, WarpId id) {
    if (id >= kWarpSlots || !machine.warps[id].allocated)
        return make_issue(IssueKind::HostError, "InvalidWarp", "Warp slot is not allocated.");
    const auto& warp = machine.warps[id];
    if (warp.status != WarpStatus::Ready)
        return make_issue(IssueKind::HostError, "WarpNotReady", "Warp cannot execute in its current state.");
    if (warp.exec == 0 || (warp.exec & ~warp.init_mask) != 0 ||
        (warp.init_mask & ~width_mask(machine.config.warp_width)) != 0 || warp.sp > kStackDepth)
        return make_issue(IssueKind::ArchitecturalFault, "InvalidWarpState", "Invalid execution mask or stack state.");
    return {};
}
}
Result<StepEffect> evaluate(const Machine& machine, WarpId id, const DecodedInst& inst) {
    auto valid = validate_warp(machine, id);
    if (!valid) return valid.error();
    const auto& warp = machine.warps[id];
    StepEffect effect;
    effect.next_pc = warp.pc + Word{4};
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        const auto& state = warp.lanes[lane];
        if (machine.config.check_undefined &&
            (!state.x_defined[inst.rs1] || (inst.op == Op::Add && !state.x_defined[inst.rs2]))) {
            auto issue = make_issue(IssueKind::Diagnostic, "UndefinedRegister", "Active lane reads an uninitialized integer register.");
            issue.lane = lane;
            return issue; // Discard all staged writes, including earlier lanes.
        }
        const Word lhs = inst.rs1 == 0 ? 0 : state.x[inst.rs1];
        const Word rhs = inst.op == Op::AddImmediate ? inst.immediate_bits : (inst.rs2 == 0 ? 0 : state.x[inst.rs2]);
        if (inst.rd != 0) effect.register_writes.push_back({lane, inst.rd, state.x[inst.rd], lhs + rhs});
    }
    return effect;
}
void commit_effect(Machine& machine, WarpId id, const StepEffect& effect) {
    auto& warp = machine.warps[id];
    for (const auto& write : effect.register_writes) {
        warp.lanes[write.lane].x[write.reg] = write.after;
        warp.lanes[write.lane].x_defined[write.reg] = true;
    }
    warp.pc = effect.next_pc; // Advance once per warp instruction, not once per lane.
}
Result<StepRecord> step_shader(Machine& machine, WarpId id) {
    auto valid = validate_warp(machine, id);
    if (!valid) return valid.error();
    auto& warp = machine.warps[id];
    auto contextualize = [&](Issue issue, std::optional<Word> raw = {}) -> Result<StepRecord> {
        issue.warp_id = id; issue.pc = warp.pc; issue.raw = raw;
        if (issue.kind == IssueKind::ArchitecturalFault) warp.status = WarpStatus::Faulted;
        return issue;
    };
    if (machine.attempted_steps >= machine.config.max_steps)
        return contextualize(make_issue(IssueKind::ModelLimit, "StepBudget", "Instruction attempt budget exhausted."));
    ++machine.attempted_steps;
    auto raw = machine.memory.fetch32(warp.pc);
    if (!raw) return contextualize(raw.error());
    auto inst = decode(raw.value());
    if (!inst) return contextualize(inst.error(), raw.value());
    auto effect = evaluate(machine, id, inst.value());
    if (!effect) return contextualize(effect.error(), raw.value());
    StepRecord record{machine.retired_steps + 1, id, warp.pc, effect.value().next_pc,
                      warp.exec, warp.exec, warp.sp, warp.sp, raw.value(), inst.value().op,
                      effect.value().register_writes};
    commit_effect(machine, id, effect.value());
    ++machine.retired_steps;
    return record;
}
} // namespace animus
