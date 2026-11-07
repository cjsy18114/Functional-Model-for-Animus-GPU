#include "model/model.hpp"
#include "model/execution.hpp"
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
    if (inst.rd >= kRegisters || inst.rs1 >= kRegisters || inst.rs2 >= kRegisters || inst.rs3 >= kRegisters)
        return make_issue(IssueKind::HostError, "InvalidDecodedRegister", "Decoded register index is out of range.");
    const auto& warp = machine.warps[id];
    // Read validation covers all active lanes before any module can submit effects.
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        const auto& state = warp.lanes[lane];
        if (machine.config.check_undefined &&
            ((inst.uses_rs1 && !state.x_defined[inst.rs1]) || (inst.uses_rs2 && !state.x_defined[inst.rs2]))) {
            auto issue = make_issue(IssueKind::Diagnostic, "UndefinedRegister", "Active lane reads an uninitialized integer register.");
            issue.lane = lane; return issue;
        }
        if (machine.config.check_undefined &&
            ((inst.uses_fs1 && !state.f_defined[inst.rs1]) || (inst.uses_fs2 && !state.f_defined[inst.rs2]) ||
             (inst.uses_fs3 && !state.f_defined[inst.rs3]))) {
            auto issue = make_issue(IssueKind::Diagnostic, "UndefinedFloatRegister", "Active lane reads an uninitialized floating register.");
            issue.lane = lane; return issue;
        }
    }
    switch (inst.instruction_class) {
    case InstructionClass::Alu: return eval_alu(machine, id, inst);
    case InstructionClass::MulDiv: return eval_muldiv(machine, id, inst);
    case InstructionClass::LoadStore: return eval_load_store(machine, id, inst);
    case InstructionClass::Control: return eval_control(machine, id, inst);
    case InstructionClass::Simt: return eval_simt(machine, id, inst);
    case InstructionClass::ThreadMask: return eval_thread_mask(machine, id, inst);
    case InstructionClass::Float: return eval_fp(machine, id, inst);
    case InstructionClass::Csr: return eval_csr(machine, id, inst);
    case InstructionClass::Environment: return eval_environment(machine, id, inst);
    case InstructionClass::Sfu: return eval_sfu(machine, id, inst);
    case InstructionClass::Graphics: return eval_graphics(machine, id, inst);
    }
    return make_issue(IssueKind::HostError, "InvalidInstructionClass", "Unknown instruction class.");
}
void commit_effect(Machine& machine, WarpId id, const StepEffect& effect) {
    auto& warp = machine.warps[id];
    for (const auto& write : effect.register_writes) {
        auto& lane = warp.lanes[write.lane];
        if (write.file == RegisterFile::Float) {
            lane.f_bits[write.reg] = write.after; lane.f_defined[write.reg] = true;
        } else {
            lane.x[write.reg] = write.after; lane.x_defined[write.reg] = true;
        }
    }
    for (const auto& write : effect.memory_writes)
        machine.memory.store_le_prechecked(write.address, write.bytes, write.after);
    for (const auto& write : effect.csr_writes)
        warp.lanes[write.lane].fcsr = static_cast<std::uint8_t>(write.after);
    if (effect.control) {
        warp.exec = effect.control->exec;
        warp.sp = effect.control->sp;
        warp.stack = effect.control->stack;
    }
    warp.pc = effect.next_pc; // Advance once per warp instruction, not once per lane.
    if (effect.outputs) warp.completed_outputs = effect.outputs;
    if (effect.next_status) warp.status = *effect.next_status;
}
Result<StepRecord> step_shader(Machine& machine, WarpId id) {
    auto valid = validate_warp(machine, id);
    if (!valid) {
        auto issue = valid.error();
        issue.warp_id = id;
        if (id < kWarpSlots && machine.warps[id].allocated) {
            issue.pc = machine.warps[id].pc;
            if (issue.kind == IssueKind::ArchitecturalFault) machine.warps[id].status = WarpStatus::Faulted;
        }
        return issue;
    }
    auto& warp = machine.warps[id];
    auto contextualize = [&](Issue issue, std::optional<Word> raw = {}) -> Result<StepRecord> {
        issue.warp_id = id; issue.pc = warp.pc; issue.raw = raw;
        if (issue.kind == IssueKind::ArchitecturalFault) warp.status = WarpStatus::Faulted;
        return issue;
    };
    if (machine.attempted_steps >= machine.config.max_steps)
        return contextualize(make_issue(IssueKind::ModelLimit, "StepBudget", "Instruction attempt budget exhausted."));
    ++machine.attempted_steps;
    auto target = check_target(machine, warp, warp.pc);
    if (!target) return contextualize(target.error());
    auto raw = machine.memory.fetch32(warp.pc);
    if (!raw) return contextualize(raw.error());
    auto inst = decode(raw.value());
    if (!inst) return contextualize(inst.error(), raw.value());
    auto effect = evaluate(machine, id, inst.value());
    if (!effect) return contextualize(effect.error(), raw.value());
    StepRecord record;
    record.step = machine.retired_steps + 1; record.warp_id = id;
    record.pc_before = warp.pc; record.pc_after = effect.value().next_pc;
    record.exec_before = warp.exec;
    record.exec_after = effect.value().control ? effect.value().control->exec : warp.exec;
    record.sp_before = warp.sp;
    record.sp_after = effect.value().control ? effect.value().control->sp : warp.sp;
    record.raw = raw.value(); record.op = inst.value().op;
    record.register_writes = effect.value().register_writes;
    record.memory_writes = effect.value().memory_writes;
    record.experimental_profile = machine.config.allow_experimental;
    record.csr_writes = effect.value().csr_writes;
    record.fp_state_policy = machine.config.fp_state_policy; record.sfu_profile = machine.config.sfu_profile;
    record.invocation_id = warp.invocation_id;
    record.outputs = effect.value().outputs;
    record.status_before = warp.status;
    record.status_after = effect.value().next_status.value_or(warp.status);
    commit_effect(machine, id, effect.value());
    ++machine.retired_steps;
    return record;
}
RunReport run_until_stop(Machine& machine, WarpId id, Addr stop_pc, std::ostream* trace) {
    const auto before = machine.retired_steps;
    auto valid = validate_warp(machine, id);
    if (!valid) return {valid.error().kind == IssueKind::ArchitecturalFault ? StopReason::ArchitecturalFault : StopReason::HostError, 0, valid.error()};
    if ((stop_pc & 3u) != 0)
        return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "InvalidStopPc", "Host stop_pc must be word aligned.")};
    while (machine.warps[id].pc != stop_pc) {
        auto result = step_shader(machine, id);
        if (!result) {
            const auto& issue = result.error();
            StopReason reason = StopReason::HostError;
            if (issue.code == "StepBudget") reason = StopReason::Timeout;
            else if (issue.kind == IssueKind::ArchitecturalFault) reason = StopReason::ArchitecturalFault;
            else if (issue.kind == IssueKind::ModelLimit) reason = StopReason::ModelLimit;
            else if (issue.kind == IssueKind::Diagnostic) reason = StopReason::Diagnostic;
            return {reason, machine.retired_steps - before, issue};
        }
        if (trace) write_trace(*trace, result.value());
        if (machine.warps[id].status == WarpStatus::Ended)
            return {StopReason::Completed, machine.retired_steps - before, {}};
    }
    return {StopReason::Paused, machine.retired_steps - before, {}};
}
} // namespace animus
