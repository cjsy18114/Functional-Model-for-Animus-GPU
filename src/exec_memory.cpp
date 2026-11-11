#include "model/execution.hpp"
#include <cstdint>

namespace animus {
Result<StepEffect> eval_load_store(const Machine& machine, WarpId id, const DecodedInst& inst) {
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    const bool store = inst.op == Op::StoreByte || inst.op == Op::StoreHalf || inst.op == Op::StoreWord || inst.op == Op::StoreFloat;
    const unsigned bytes = (inst.op == Op::LoadByte || inst.op == Op::LoadByteUnsigned || inst.op == Op::StoreByte) ? 1 :
                           (inst.op == Op::LoadHalf || inst.op == Op::LoadHalfUnsigned || inst.op == Op::StoreHalf) ? 2 : 4;
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        // RV32 effective-address addition wraps modulo 2^32. The full transfer must still fit.
        const Addr address = read_x(warp, lane, inst.rs1) + inst.immediate_bits;
        if (address % bytes != 0) {
            auto issue = make_issue(machine.config.allow_experimental ? IssueKind::ArchitecturalFault : IssueKind::ModelLimit,
                "DataAlignment", "S06.1: unaligned data access; experimental profile requires natural alignment.");
            issue.lane = lane; issue.address = address; issue.spec_issue_id = "S06.1";
            return issue;
        }
        auto checked = machine.memory.check_access(address, bytes, store ? Access::Write : Access::Read);
        if (!checked) { auto issue = checked.error(); issue.lane = lane; issue.address = address; return issue; }
        auto resource = check_submission_access(machine, address, bytes, store ? Access::Write : Access::Read);
        if (!resource) { auto issue = resource.error(); issue.lane = lane; issue.address = address; return issue; }
        if (store) {
            for (const auto& old : effect.memory_writes) {
                if (std::uint64_t{address} < std::uint64_t{old.address} + old.bytes &&
                    std::uint64_t{old.address} < std::uint64_t{address} + bytes && !machine.config.allow_experimental) {
                    auto issue = make_issue(IssueKind::ModelLimit, "StoreConflict", "S07: overlapping lane stores require an explicit experimental profile.");
                    issue.lane = lane; issue.address = address; issue.spec_issue_id = "S07";
                    return issue;
                }
            }
            Word value = inst.op == Op::StoreFloat ? warp.lanes[lane].f_bits[inst.rs2] : read_x(warp, lane, inst.rs2);
            if (bytes < 4) value &= (Word{1} << (bytes * 8)) - 1;
            // Peek is an internal snapshot, not an extra guest read (write-only mappings are valid).
            effect.memory_writes.push_back({lane, address, bytes, machine.memory.peek_le_prechecked(address, bytes), value});
        } else {
            Word value = machine.memory.peek_le_prechecked(address, bytes);
            if (inst.op == Op::LoadByte || inst.op == Op::LoadHalf) value = sign_extend_bits(value, bytes * 8);
            if (inst.op == Op::LoadFloat) append_f_write(effect, warp, lane, inst.rd, value);
            else append_x_write(effect, warp, lane, inst.rd, value); // LW x0 still performs access checks.
        }
    }
    return effect;
}
} // namespace animus
