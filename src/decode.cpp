#include "model/model.hpp"
#include "model/execution.hpp"

namespace animus {
namespace {
Issue illegal() { return make_issue(IssueKind::ArchitecturalFault, "IllegalEncoding", "Reserved or malformed instruction encoding."); }
}
Result<DecodedInst> decode(Word raw) {
    if ((raw & 3u) != 3u || (raw & 0x7Fu) == 0x7Fu) return illegal();
    DecodedInst inst;
    inst.raw = raw;
    inst.rd = (raw >> 7) & 31u;
    inst.rs1 = (raw >> 15) & 31u;
    const unsigned rs2 = (raw >> 20) & 31u;
    const auto opcode = raw & 0x7Fu;
    const auto funct3 = (raw >> 12) & 7u;
    const auto funct7 = raw >> 25;
    if (opcode == 0x13) {
        inst.uses_rs1 = true;
        inst.immediate_bits = sign_extend_bits(raw >> 20, 12);
        switch (funct3) {
        case 0: inst.op = Op::AddImmediate; break;
        case 2: inst.op = Op::LessSignedImmediate; break;
        case 3: inst.op = Op::LessUnsignedImmediate; break;
        case 4: inst.op = Op::XorImmediate; break;
        case 6: inst.op = Op::OrImmediate; break;
        case 7: inst.op = Op::AndImmediate; break;
        case 1:
            if (funct7 != 0) return illegal();
            inst.op = Op::ShiftLeftImmediate; inst.immediate_bits = rs2; break;
        case 5:
            if (funct7 != 0 && funct7 != 0x20) return illegal();
            inst.op = funct7 == 0 ? Op::ShiftRightImmediate : Op::ShiftRightArithmeticImmediate;
            inst.immediate_bits = rs2; break;
        }
        return inst;
    }
    if (opcode == 0x33) {
        inst.uses_rs1 = inst.uses_rs2 = true; inst.rs2 = rs2;
        if (funct7 == 1) {
            inst.instruction_class = InstructionClass::MulDiv;
            constexpr Op ops[] = {Op::Multiply, Op::MultiplyHighSigned, Op::MultiplyHighSignedUnsigned,
                Op::MultiplyHighUnsigned, Op::DivideSigned, Op::DivideUnsigned, Op::RemainderSigned, Op::RemainderUnsigned};
            inst.op = ops[funct3];
        } else if (funct7 == 0) {
            constexpr Op ops[] = {Op::Add, Op::ShiftLeft, Op::LessSigned, Op::LessUnsigned,
                                 Op::Xor, Op::ShiftRight, Op::Or, Op::And};
            inst.op = ops[funct3];
        } else if (funct7 == 0x20 && (funct3 == 0 || funct3 == 5)) {
            inst.op = funct3 == 0 ? Op::Subtract : Op::ShiftRightArithmetic;
        } else return illegal();
        return inst;
    }
    if (opcode == 0x37 || opcode == 0x17) {
        inst.op = opcode == 0x37 ? Op::Lui : Op::Auipc;
        inst.rs1 = 0; inst.immediate_bits = raw & 0xFFFFF000u;
        return inst;
    }
    if (opcode == 0x03 || opcode == 0x23) {
        inst.instruction_class = InstructionClass::LoadStore;
        inst.uses_rs1 = true;
        if (opcode == 0x03) {
            inst.immediate_bits = sign_extend_bits(raw >> 20, 12);
            switch (funct3) {
            case 0: inst.op = Op::LoadByte; break;
            case 1: inst.op = Op::LoadHalf; break;
            case 2: inst.op = Op::LoadWord; break;
            case 4: inst.op = Op::LoadByteUnsigned; break;
            case 5: inst.op = Op::LoadHalfUnsigned; break;
            default: return illegal();
            }
        } else {
            if (funct3 > 2) return illegal();
            inst.op = funct3 == 0 ? Op::StoreByte : funct3 == 1 ? Op::StoreHalf : Op::StoreWord;
            inst.rs2 = rs2; inst.rd = 0; inst.uses_rs2 = true;
            inst.immediate_bits = sign_extend_bits(((raw >> 25) << 5) | ((raw >> 7) & 31u), 12);
        }
        return inst;
    }
    if (opcode == 0x63) {
        inst.instruction_class = InstructionClass::Control;
        inst.uses_rs1 = inst.uses_rs2 = true; inst.rs2 = rs2; inst.rd = 0;
        switch (funct3) {
        case 0: inst.op = Op::BranchEqual; break;
        case 1: inst.op = Op::BranchNotEqual; break;
        case 4: inst.op = Op::BranchLessSigned; break;
        case 5: inst.op = Op::BranchGreaterEqualSigned; break;
        case 6: inst.op = Op::BranchLessUnsigned; break;
        case 7: inst.op = Op::BranchGreaterEqualUnsigned; break;
        default: return illegal();
        }
        const Word offset = ((raw >> 31) << 12) | (((raw >> 7) & 1u) << 11) |
                            (((raw >> 25) & 63u) << 5) | (((raw >> 8) & 15u) << 1);
        inst.immediate_bits = sign_extend_bits(offset, 13);
        return inst;
    }
    if (opcode == 0x6F || opcode == 0x67) {
        inst.instruction_class = InstructionClass::Control;
        if (opcode == 0x6F) {
            inst.op = Op::JumpLink; inst.rs1 = 0;
            const Word offset = ((raw >> 31) << 20) | (raw & 0xFF000u) |
                                (((raw >> 20) & 1u) << 11) | (((raw >> 21) & 1023u) << 1);
            inst.immediate_bits = sign_extend_bits(offset, 21);
        } else {
            if (funct3 != 0) return illegal();
            inst.op = Op::JumpLinkRegister; inst.uses_rs1 = true;
            inst.immediate_bits = sign_extend_bits(raw >> 20, 12);
        }
        return inst;
    }
    if (opcode == 0x0B && (funct7 == 2 || funct7 == 3)) {
        if (funct3 != 0 || inst.rd != 0) return illegal();
        inst.instruction_class = InstructionClass::Simt;
        if (funct7 == 2) {
            inst.op = Op::Split; inst.uses_rs1 = inst.uses_rs2 = true; inst.rs2 = rs2;
        } else {
            if (inst.rs1 != 0 || rs2 != 0) return illegal();
            inst.op = Op::Join;
        }
        return inst;
    }
    if (opcode == 0x73 && (raw >> 20) == 0x800) {
        if (funct3 == 0 || funct3 == 4) return illegal();
        inst.op = Op::ReadThreadMask; inst.instruction_class = InstructionClass::ThreadMask;
        inst.csr_operation = funct3; // rs1 stores the source index or zimm, not its runtime value.
        return inst;
    }
    return make_issue(IssueKind::ModelLimit, "InstructionNotImplemented", "Instruction is outside stage B; full encoding validation is deferred.");
}
const char* op_name(Op op) {
    switch (op) {
#define NAME(opcode, text) case Op::opcode: return text;
    NAME(AddImmediate,"ADDI") NAME(Add,"ADD") NAME(Subtract,"SUB")
    NAME(ShiftLeft,"SLL") NAME(LessSigned,"SLT") NAME(LessUnsigned,"SLTU") NAME(Xor,"XOR")
    NAME(ShiftRight,"SRL") NAME(ShiftRightArithmetic,"SRA") NAME(Or,"OR") NAME(And,"AND")
    NAME(LessSignedImmediate,"SLTI") NAME(LessUnsignedImmediate,"SLTIU") NAME(XorImmediate,"XORI")
    NAME(OrImmediate,"ORI") NAME(AndImmediate,"ANDI") NAME(ShiftLeftImmediate,"SLLI")
    NAME(ShiftRightImmediate,"SRLI") NAME(ShiftRightArithmeticImmediate,"SRAI") NAME(Lui,"LUI") NAME(Auipc,"AUIPC")
    NAME(LoadByte,"LB") NAME(LoadHalf,"LH") NAME(LoadWord,"LW") NAME(LoadByteUnsigned,"LBU") NAME(LoadHalfUnsigned,"LHU")
    NAME(StoreByte,"SB") NAME(StoreHalf,"SH") NAME(StoreWord,"SW")
    NAME(BranchEqual,"BEQ") NAME(BranchNotEqual,"BNE") NAME(BranchLessSigned,"BLT") NAME(BranchGreaterEqualSigned,"BGE")
    NAME(BranchLessUnsigned,"BLTU") NAME(BranchGreaterEqualUnsigned,"BGEU") NAME(JumpLink,"JAL") NAME(JumpLinkRegister,"JALR")
    NAME(Multiply,"MUL") NAME(MultiplyHighSigned,"MULH") NAME(MultiplyHighSignedUnsigned,"MULHSU") NAME(MultiplyHighUnsigned,"MULHU")
    NAME(DivideSigned,"DIV") NAME(DivideUnsigned,"DIVU") NAME(RemainderSigned,"REM") NAME(RemainderUnsigned,"REMU")
    NAME(Split,"gpu.split") NAME(Join,"gpu.join") NAME(ReadThreadMask,"gpu_tmr")
#undef NAME
    }
    return "UNKNOWN";
}
} // namespace animus
