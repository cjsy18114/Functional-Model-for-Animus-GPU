#include "model/execution.hpp"
#include <cstdint>

namespace animus {
void append_f_write(StepEffect& effect, const WarpState& warp, unsigned lane, unsigned reg, Word bits) {
    effect.register_writes.push_back({lane, reg, warp.lanes[lane].f_bits[reg], bits, RegisterFile::Float});
}
namespace {
bool pure_bits(Op op) {
    return op == Op::MoveFloatToInt || op == Op::MoveIntToFloat || op == Op::FloatClassify ||
           op == Op::FloatSignInject || op == Op::FloatSignInvert || op == Op::FloatSignXor;
}
bool uses_rounding(Op op) {
    switch (op) {
    case Op::FloatAdd: case Op::FloatSubtract: case Op::FloatMultiply: case Op::FloatDivide: case Op::FloatSqrt:
    case Op::FloatMultiplyAdd: case Op::FloatMultiplySubtract: case Op::FloatNegMultiplySubtract: case Op::FloatNegMultiplyAdd:
    case Op::FloatToInt: case Op::FloatToUnsigned: case Op::IntToFloat: case Op::UnsignedToFloat: return true;
    default: return false;
    }
}
bool integer_destination(Op op) {
    return op == Op::MoveFloatToInt || op == Op::FloatClassify || op == Op::FloatToInt ||
           op == Op::FloatToUnsigned || op == Op::FloatEqual || op == Op::FloatLess || op == Op::FloatLessEqual;
}
}
Result<StepEffect> eval_fp(const Machine& machine, WarpId id, const DecodedInst& inst) {
    if (!pure_bits(inst.op) && machine.config.fp_state_policy != FpStatePolicy::PerLaneExperimental) {
        auto issue = make_issue(IssueKind::ModelLimit, "FpStatePolicyRequired", "S04: FP flags/rounding state needs an explicit per-lane policy.");
        issue.spec_issue_id = "S04"; return issue;
    }
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        const auto& state = warp.lanes[lane];
        unsigned rounding = 0;
        if (uses_rounding(inst.op)) {
            auto resolved = resolve_rm(inst.rm, state.fcsr >> 5);
            if (!resolved) { auto issue = resolved.error(); issue.lane = lane; return issue; }
            rounding = resolved.value();
        }
        const Word a = inst.uses_rs1 ? read_x(warp, lane, inst.rs1) : state.f_bits[inst.rs1];
        const Word b = inst.uses_fs2 ? state.f_bits[inst.rs2] : 0;
        const Word c = inst.uses_fs3 ? state.f_bits[inst.rs3] : 0;
        auto computed = fp_eval(inst.op, a, b, c, rounding);
        if (!computed) return computed.error();
        if (integer_destination(inst.op)) append_x_write(effect, warp, lane, inst.rd, computed.value().bits);
        else append_f_write(effect, warp, lane, inst.rd, computed.value().bits);
        if (!pure_bits(inst.op)) {
            const Word next = state.fcsr | computed.value().flags;
            if (next != state.fcsr) effect.csr_writes.push_back({lane, 0x003, state.fcsr, next});
        }
    }
    return effect;
}
Result<StepEffect> eval_sfu(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    // All active lane results are computed in one logical step, irrespective of physical SFU count.
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        auto result = sfu_eval(inst.op, warp.lanes[lane].f_bits[inst.rs1], machine.config.sfu_profile);
        if (!result) return result.error();
        append_f_write(effect, warp, lane, inst.rd, result.value());
    }
    return effect;
}
} // namespace animus
