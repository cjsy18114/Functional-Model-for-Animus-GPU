#include "model/execution.hpp"
#include <cstdint>
#include <optional>

namespace animus {
Word sign_extend_bits(Word value, unsigned width) {
    if (width == 32) return value;
    const Word mask = (Word{1} << width) - 1;
    value &= mask;
    return (value & (Word{1} << (width - 1))) ? value | ~mask : value;
}
std::int64_t signed_value(Word value) {
    return (value & 0x80000000u) ? std::int64_t{value} - (1ll << 32) : std::int64_t{value};
}
Word read_x(const WarpState& warp, unsigned lane, unsigned reg) {
    return reg == 0 ? 0 : warp.lanes[lane].x[reg];
}
void append_x_write(StepEffect& effect, const WarpState& warp, unsigned lane, unsigned reg, Word value) {
    if (reg != 0) effect.register_writes.push_back({lane, reg, warp.lanes[lane].x[reg], value});
}
namespace {
Word arithmetic_shift(Word value, unsigned shift) {
    // Implement sign fill in unsigned arithmetic: no implementation-defined signed right shift.
    if (shift == 0) return value;
    return (value >> shift) | ((value & 0x80000000u) ? (0xFFFFFFFFu << (32 - shift)) : 0u);
}
Issue internal_op() { return make_issue(IssueKind::HostError, "InvalidDecodedOp", "Wrong operation for execution module."); }
}
Result<StepEffect> eval_alu(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        const Word a = inst.uses_rs1 ? read_x(warp, lane, inst.rs1) : 0;
        const Word b = inst.uses_rs2 ? read_x(warp, lane, inst.rs2) : inst.immediate_bits;
        const unsigned shift = b & 31u;
        Word result = 0;
        switch (inst.op) {
        case Op::Add: case Op::AddImmediate: result = a + b; break;
        case Op::Subtract: result = a - b; break;
        case Op::ShiftLeft: case Op::ShiftLeftImmediate: result = a << shift; break;
        case Op::ShiftRight: case Op::ShiftRightImmediate: result = a >> shift; break;
        case Op::ShiftRightArithmetic: case Op::ShiftRightArithmeticImmediate: result = arithmetic_shift(a, shift); break;
        case Op::LessSigned: case Op::LessSignedImmediate: result = signed_value(a) < signed_value(b); break;
        case Op::LessUnsigned: case Op::LessUnsignedImmediate: result = a < b; break;
        case Op::Xor: case Op::XorImmediate: result = a ^ b; break;
        case Op::Or: case Op::OrImmediate: result = a | b; break;
        case Op::And: case Op::AndImmediate: result = a & b; break;
        case Op::Lui: result = inst.immediate_bits; break;
        case Op::Auipc: result = warp.pc + inst.immediate_bits; break;
        default: return internal_op();
        }
        append_x_write(effect, warp, lane, inst.rd, result);
    }
    return effect;
}
Result<StepEffect> eval_muldiv(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        const Word a = read_x(warp, lane, inst.rs1), b = read_x(warp, lane, inst.rs2);
        const auto sa = signed_value(a), sb = signed_value(b);
        Word result = 0;
        // 32x32 products fit in uint64/int64 as appropriate. Signed->unsigned conversion is modulo 2^64.
        switch (inst.op) {
        case Op::Multiply: result = static_cast<Word>(std::uint64_t{a} * b); break;
        case Op::MultiplyHighSigned: result = static_cast<Word>(static_cast<std::uint64_t>(sa * sb) >> 32); break;
        case Op::MultiplyHighSignedUnsigned: result = static_cast<Word>(static_cast<std::uint64_t>(sa * std::int64_t{b}) >> 32); break;
        case Op::MultiplyHighUnsigned: result = static_cast<Word>((std::uint64_t{a} * b) >> 32); break;
        case Op::DivideSigned: result = b == 0 ? 0xFFFFFFFFu : static_cast<Word>(sa / sb); break;
        case Op::DivideUnsigned: result = b == 0 ? 0xFFFFFFFFu : a / b; break;
        case Op::RemainderSigned: result = b == 0 ? a : static_cast<Word>(sa % sb); break;
        case Op::RemainderUnsigned: result = b == 0 ? a : a % b; break;
        default: return internal_op();
        }
        // INT32_MIN / -1 is evaluated in int64, then converted modulo 2^32 safely.
        append_x_write(effect, warp, lane, inst.rd, result);
    }
    return effect;
}
Result<void> check_target(const Machine& machine, const WarpState& warp, Addr target) {
    auto result = machine.memory.fetch32(target);
    if (!result) { auto issue = result.error(); issue.address = target; return issue; }
    for (const auto& region : warp.code_regions)
        if (target >= region.base && std::uint64_t{target} + 4 <= std::uint64_t{region.base} + region.bytes) return {};
    auto issue = make_issue(IssueKind::ArchitecturalFault, "ShaderCodeRange", "Target is outside current shader code ranges.");
    issue.address = target; return issue;
}
Result<void> check_target_alignment(Addr target) {
    if ((target & 3u) == 0) return {};
    auto issue = make_issue(IssueKind::ArchitecturalFault, "InstructionAlignment", "Target must be 4-byte aligned.");
    issue.address = target; return issue;
}
Result<StepEffect> eval_control(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    std::optional<bool> decision;
    std::optional<Addr> indirect_target;
    const bool jump = inst.op == Op::JumpLink || inst.op == Op::JumpLinkRegister;
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        const Word a = inst.uses_rs1 ? read_x(warp, lane, inst.rs1) : 0;
        const Word b = inst.uses_rs2 ? read_x(warp, lane, inst.rs2) : 0;
        bool taken = true;
        switch (inst.op) {
        case Op::BranchEqual: taken = a == b; break;
        case Op::BranchNotEqual: taken = a != b; break;
        case Op::BranchLessSigned: taken = signed_value(a) < signed_value(b); break;
        case Op::BranchGreaterEqualSigned: taken = signed_value(a) >= signed_value(b); break;
        case Op::BranchLessUnsigned: taken = a < b; break;
        case Op::BranchGreaterEqualUnsigned: taken = a >= b; break;
        case Op::JumpLink: break;
        case Op::JumpLinkRegister: {
            const Addr target = (a + inst.immediate_bits) & ~Word{1};
            if (indirect_target && *indirect_target != target)
                return make_issue(IssueKind::ArchitecturalFault, "NonUniformTarget", "Active lanes have different indirect jump targets.");
            indirect_target = target;
            break;
        }
        default: return internal_op();
        }
        if (decision && *decision != taken)
            return make_issue(IssueKind::ArchitecturalFault, "DivergentBranch", "Ordinary branch decisions differ between active lanes.");
        decision = taken;
        if (jump) append_x_write(effect, warp, lane, inst.rd, warp.pc + Word{4});
    }
    if (decision.value()) {
        const Addr target = indirect_target.value_or(warp.pc + inst.immediate_bits);
        // RV32I: target misalignment faults on the jump; access faults occur on the later fetch.
        auto checked = check_target_alignment(target);
        if (!checked) return checked.error(); // No link-register writes commit on misalignment.
        effect.next_pc = target;
    }
    return effect;
}
} // namespace animus
