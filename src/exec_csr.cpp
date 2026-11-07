#include "model/execution.hpp"
#include <cstdint>

namespace animus {
Result<Word> read_csr(const Machine& machine, WarpId id, unsigned lane, std::uint16_t address) {
    if (id >= kWarpSlots || !machine.warps[id].allocated || lane >= machine.config.warp_width)
        return make_issue(IssueKind::HostError, "InvalidCsrLocation", "CSR location is outside an allocated warp.");
    if (address == 0x800) return machine.warps[id].exec & width_mask(machine.config.warp_width);
    if (address < 1 || address > 3)
        return make_issue(IssueKind::ModelLimit, "CsrNotImplemented", "This execution environment exposes fflags/frm/fcsr and gpu_tmr only.");
    if (machine.config.fp_state_policy != FpStatePolicy::PerLaneExperimental) {
        auto issue = make_issue(IssueKind::ModelLimit, "FpStatePolicyRequired", "S04: floating CSR scope is not selected.");
        issue.spec_issue_id = "S04"; return issue;
    }
    const Word value = machine.warps[id].lanes[lane].fcsr;
    return address == 1 ? value & 31u : address == 2 ? value >> 5 : value;
}
Result<StepEffect> eval_csr(const Machine& machine, WarpId id, const DecodedInst& inst) {
    if (inst.csr < 1 || inst.csr > 3)
        return make_issue(IssueKind::ModelLimit, "CsrNotImplemented", "Unknown CSR in the current execution environment.");
    if (machine.config.fp_state_policy != FpStatePolicy::PerLaneExperimental) {
        auto issue = make_issue(IssueKind::ModelLimit, "FpStatePolicyRequired", "S04: select a floating CSR scope before CSR access.");
        issue.spec_issue_id = "S04"; return issue;
    }
    const auto& warp = machine.warps[id];
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    const unsigned operation = inst.csr_operation;
    const bool exchange = operation == 1 || operation == 5;
    const bool read = !exchange || inst.rd != 0;
    const bool write = exchange || inst.rs1 != 0;
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if ((warp.exec & (Mask{1} << lane)) == 0) continue;
        // A suppressed CSRRW read does not call read_csr. A zero source *index* suppresses CSRRS/RC writes.
        Word old = 0;
        if (read) { auto value = read_csr(machine, id, lane, inst.csr); if (!value) return value.error(); old = value.value(); }
        if (read) append_x_write(effect, warp, lane, inst.rd, old);
        if (write) {
            const Word source = operation >= 5 ? inst.rs1 : read_x(warp, lane, inst.rs1);
            const Word value = exchange ? source : (operation == 2 || operation == 6 ? old | source : old & ~source);
            const Word prior = warp.lanes[lane].fcsr;
            // S13: preserve other fields, mask reserved high bits, retain even reserved frm values.
            const Word next = inst.csr == 1 ? (prior & 0xE0u) | (value & 31u) :
                              inst.csr == 2 ? (prior & 31u) | ((value & 7u) << 5) : value & 255u;
            effect.csr_writes.push_back({lane, inst.csr, prior, next}); // before/after are full fcsr snapshots.
        }
    }
    return effect;
}
Result<StepEffect> eval_environment(const Machine& machine, WarpId id, const DecodedInst& inst) {
    if (inst.op == Op::EnvironmentCall || inst.op == Op::Breakpoint) {
        auto issue = make_issue(IssueKind::ArchitecturalFault,
            inst.op == Op::EnvironmentCall ? "EnvironmentCall" : "Breakpoint",
            "S08: environment exception reported; no handler ABI or implicit successful exit is assumed.");
        issue.spec_issue_id = "S08"; return issue;
    }
    StepEffect effect; effect.next_pc = machine.warps[id].pc + Word{4};
    // All earlier accesses already completed in this sequential functional memory model.
    // FENCE still decodes, retires and appears in traces; no cache or hardware delay is modeled.
    return effect;
}
} // namespace animus
