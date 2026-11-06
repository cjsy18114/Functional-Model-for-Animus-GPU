#include "model/model.hpp"

namespace animus {
Result<DecodedInst> decode(Word raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.rd = (raw >> 7) & 31u;
    inst.rs1 = (raw >> 15) & 31u;
    const auto opcode = raw & 0x7Fu;
    const auto funct3 = (raw >> 12) & 7u;
    if (opcode == 0x13 && funct3 == 0) {
        inst.op = Op::AddImmediate;
        const Word immediate = raw >> 20;
        inst.immediate_bits = (immediate & 0x800u) ? immediate | 0xFFFFF000u : immediate;
        return inst;
    }
    if (opcode == 0x33 && funct3 == 0 && (raw >> 25) == 0) {
        inst.op = Op::Add;
        inst.rs2 = (raw >> 20) & 31u;
        return inst;
    }
    if ((raw & 3u) != 3u || opcode == 0 || opcode == 0x7Fu)
        return make_issue(IssueKind::ArchitecturalFault, "IllegalEncoding", "Not a supported 32-bit encoding.");
    // Other encodings are not fully validated yet; never claim they are ISA-illegal.
    return make_issue(IssueKind::ModelLimit, "InstructionNotImplemented", "Stage A executes ADDI and ADD only; this encoding needs later decoding.");
}
const char* op_name(Op op) { return op == Op::Add ? "ADD" : "ADDI"; }
} // namespace animus
