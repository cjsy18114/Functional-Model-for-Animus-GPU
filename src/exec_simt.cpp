#include "model/execution.hpp"
#include <optional>

namespace animus {
namespace {
Issue simt_issue(const char* code, const char* message) {
    return make_issue(IssueKind::ArchitecturalFault, code, message);
}
}
Result<StepEffect> eval_simt(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    ControlUpdate update{warp.exec, warp.sp, warp.stack};
    if (inst.op == Op::Split) {
        if (warp.sp >= kStackDepth) return simt_issue("StackOverflow", "split exceeds eight live frames.");
        Mask true_mask = 0;
        std::optional<Addr> else_pc;
        for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
            const Mask bit = Mask{1} << lane;
            if ((warp.exec & bit) == 0) continue;
            const Addr target = read_x(warp, lane, inst.rs2);
            if (else_pc && *else_pc != target) return simt_issue("NonUniformTarget", "split false-entry address must be uniform in active lanes.");
            else_pc = target;
            if (read_x(warp, lane, inst.rs1) != 0) true_mask |= bit;
        }
        // S12: check alignment here; code-range/access errors are detected when that path is fetched.
        auto checked = check_target_alignment(else_pc.value());
        if (!checked) return checked.error();
        const Mask false_mask = warp.exec & ~true_mask;
        auto& frame = update.stack[update.sp++];
        frame = {warp.exec, false_mask, else_pc.value(), 0, StackPhase::Single};
        if (true_mask != 0 && false_mask != 0) {
            frame.phase = StackPhase::First; update.exec = true_mask;
        } else if (true_mask == 0) {
            effect.next_pc = frame.else_pc;
        }
    } else if (inst.op == Op::Join) {
        if (warp.sp == 0) return simt_issue("StackUnderflow", "join has no matching frame.");
        auto& frame = update.stack[warp.sp - 1];
        switch (frame.phase) {
        case StackPhase::Single:
            if (warp.exec != frame.entry_mask) return simt_issue("MaskMismatch", "SINGLE join mask differs from entry mask.");
            update.exec = frame.entry_mask; --update.sp;
            break;
        case StackPhase::First: {
            if (warp.exec != (frame.entry_mask & ~frame.else_mask) || frame.else_mask == 0)
                return simt_issue("MaskMismatch", "FIRST join requires the saved true mask.");
            auto checked = check_target_alignment(frame.else_pc);
            if (!checked) return checked.error();
            frame.join_pc = warp.pc; frame.phase = StackPhase::Second;
            update.exec = frame.else_mask; effect.next_pc = frame.else_pc;
            break;
        }
        case StackPhase::Second:
            if (warp.pc != frame.join_pc) return simt_issue("JoinAddressMismatch", "Both paths must reach the same static join instruction.");
            if (warp.exec != frame.else_mask) return simt_issue("MaskMismatch", "SECOND join requires the saved false mask.");
            update.exec = frame.entry_mask; --update.sp;
            break;
        default: return simt_issue("InvalidStackPhase", "Reserved SIMT stack phase.");
        }
    } else return make_issue(IssueKind::HostError, "InvalidDecodedOp", "Not a split/join operation.");
    effect.control = update;
    return effect;
}
Result<StepEffect> eval_thread_mask(const Machine& machine, WarpId id, const DecodedInst& inst) {
    // CSRRS/CSRRC and their immediate forms suppress writes ONLY for source index/zimm zero.
    // CSRRW/CSRRWI always attempt a write, even with zero source or rd=x0.
    const bool write = inst.csr_operation == 1 || inst.csr_operation == 5 || inst.rs1 != 0;
    if (write) return simt_issue("ReadOnlyCsr", "gpu_tmr is read-only; CSR write request rejected.");
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane)
        if ((warp.exec & (Mask{1} << lane)) != 0)
            append_x_write(effect, warp, lane, inst.rd, warp.exec & width_mask(machine.config.warp_width));
    return effect;
}
} // namespace animus
