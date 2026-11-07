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
    if (opcode == 0x2B) {
        inst.instruction_class = InstructionClass::Graphics;
        if (funct3 == 0) {
            if (inst.rs1 != 0) return illegal();
            inst.op = Op::GraphicsGet;
            inst.selector = static_cast<std::uint16_t>(raw >> 20);
        } else if (funct3 == 2) {
            if (funct7 != 0) return illegal(); // Reserved rs3 and texture slot must both be encoded zero.
            inst.op = Op::GraphicsTexture; inst.rs2 = rs2;
            inst.uses_rs1 = inst.uses_rs2 = true;
        } else if (funct3 == 3) {
            if (raw != 0x0000302Bu) return illegal();
            inst.op = Op::GraphicsEnd; // Fixed output registers are implicit sources, checked at execution.
        } else return illegal(); // Includes the explicitly reserved funct3=001.
        return inst;
    }
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
    if (opcode == 0x07 || opcode == 0x27) {
        if (funct3 != 2) return illegal(); // FP32 only.
        inst.instruction_class = InstructionClass::LoadStore; inst.uses_rs1 = true;
        if (opcode == 0x07) {
            inst.op = Op::LoadFloat; inst.immediate_bits = sign_extend_bits(raw >> 20, 12);
        } else {
            inst.op = Op::StoreFloat; inst.rs2 = rs2; inst.rd = 0; inst.uses_fs2 = true;
            inst.immediate_bits = sign_extend_bits(((raw >> 25) << 5) | ((raw >> 7) & 31u), 12);
        }
        return inst;
    }
    if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
        if (((raw >> 25) & 3u) != 0 || funct3 == 5 || funct3 == 6) return illegal();
        inst.instruction_class = InstructionClass::Float;
        inst.op = opcode == 0x43 ? Op::FloatMultiplyAdd : opcode == 0x47 ? Op::FloatMultiplySubtract :
                  opcode == 0x4B ? Op::FloatNegMultiplySubtract : Op::FloatNegMultiplyAdd;
        inst.rs2 = rs2; inst.rs3 = raw >> 27; inst.rm = funct3;
        inst.uses_fs1 = inst.uses_fs2 = inst.uses_fs3 = true;
        return inst;
    }
    if (opcode == 0x53) {
        inst.instruction_class = InstructionClass::Float; inst.rs2 = rs2;
        inst.uses_fs1 = true; inst.rm = funct3;
        bool rounding = false;
        switch (funct7) {
        case 0: inst.op = Op::FloatAdd; inst.uses_fs2 = true; rounding = true; break;
        case 4: inst.op = Op::FloatSubtract; inst.uses_fs2 = true; rounding = true; break;
        case 8: inst.op = Op::FloatMultiply; inst.uses_fs2 = true; rounding = true; break;
        case 12: inst.op = Op::FloatDivide; inst.uses_fs2 = true; rounding = true; break;
        case 44:
            if (rs2 != 0) return illegal();
            inst.op = Op::FloatSqrt; rounding = true; break;
        case 16:
            if (funct3 > 2) return illegal();
            inst.op = funct3 == 0 ? Op::FloatSignInject : funct3 == 1 ? Op::FloatSignInvert : Op::FloatSignXor;
            inst.uses_fs2 = true; break;
        case 20:
            if (funct3 > 1) return illegal();
            inst.op = funct3 == 0 ? Op::FloatMin : Op::FloatMax; inst.uses_fs2 = true; break;
        case 80:
            if (funct3 > 2) return illegal();
            inst.op = funct3 == 0 ? Op::FloatLessEqual : funct3 == 1 ? Op::FloatLess : Op::FloatEqual;
            inst.uses_fs2 = true; break;
        case 96:
            if (rs2 > 1) return illegal();
            inst.op = rs2 == 0 ? Op::FloatToInt : Op::FloatToUnsigned; rounding = true; break;
        case 104:
            if (rs2 > 1) return illegal();
            inst.op = rs2 == 0 ? Op::IntToFloat : Op::UnsignedToFloat;
            inst.uses_fs1 = false; inst.uses_rs1 = true; rounding = true; break;
        case 112:
            if (rs2 != 0 || funct3 > 1) return illegal();
            inst.op = funct3 == 0 ? Op::MoveFloatToInt : Op::FloatClassify; break;
        case 120:
            if (rs2 != 0 || funct3 != 0) return illegal();
            inst.op = Op::MoveIntToFloat; inst.uses_fs1 = false; inst.uses_rs1 = true; break;
        default: return illegal();
        }
        if (rounding && (funct3 == 5 || funct3 == 6)) return illegal();
        return inst;
    }
    if (opcode == 0x0B && funct7 >= 32 && funct7 <= 37) {
        if (funct3 != 0 || rs2 != 0) return illegal();
        constexpr Op ops[] = {Op::SfuReciprocal, Op::SfuReciprocalSqrt, Op::SfuExp2, Op::SfuLog2, Op::SfuSin, Op::SfuCos};
        inst.op = ops[funct7 - 32]; inst.instruction_class = InstructionClass::Sfu; inst.uses_fs1 = true;
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
        inst.csr = 0x800;
        inst.csr_operation = funct3; // rs1 stores the source index or zimm, not its runtime value.
        return inst;
    }
    if (opcode == 0x73 && funct3 != 0) {
        if (funct3 == 4) return illegal();
        inst.instruction_class = InstructionClass::Csr; inst.csr = static_cast<std::uint16_t>(raw >> 20);
        inst.csr_operation = funct3;
        switch (funct3) {
        case 1: inst.op = Op::CsrReadWrite; break;
        case 2: inst.op = Op::CsrSet; break;
        case 3: inst.op = Op::CsrClear; break;
        case 5: inst.op = Op::CsrReadWriteImmediate; break;
        case 6: inst.op = Op::CsrSetImmediate; break;
        case 7: inst.op = Op::CsrClearImmediate; break;
        default: return illegal();
        }
        inst.uses_rs1 = funct3 < 4 && (funct3 == 1 || inst.rs1 != 0);
        return inst;
    }
    if (opcode == 0x73 && (raw >> 20) <= 1) {
        if (funct3 != 0 || inst.rd != 0 || inst.rs1 != 0) return illegal();
        inst.instruction_class = InstructionClass::Environment;
        inst.op = (raw >> 20) == 0 ? Op::EnvironmentCall : Op::Breakpoint;
        return inst;
    }
    if (opcode == 0x0F && funct3 == 0) {
        // RV32I requires base implementations to ignore rd/rs1 and treat reserved
        // fm/pred/succ configurations as normal FENCE (also conservative for TSO).
        inst.op = Op::Fence; inst.instruction_class = InstructionClass::Environment; inst.rs1 = 0;
        return inst;
    }
    return make_issue(IssueKind::ModelLimit, "InstructionNotImplemented", "Instruction is outside stage D; full encoding validation is deferred.");
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
    NAME(LoadFloat,"FLW") NAME(StoreFloat,"FSW") NAME(FloatAdd,"FADD.S") NAME(FloatSubtract,"FSUB.S")
    NAME(FloatMultiply,"FMUL.S") NAME(FloatDivide,"FDIV.S") NAME(FloatSqrt,"FSQRT.S")
    NAME(FloatMultiplyAdd,"FMADD.S") NAME(FloatMultiplySubtract,"FMSUB.S")
    NAME(FloatNegMultiplySubtract,"FNMSUB.S") NAME(FloatNegMultiplyAdd,"FNMADD.S")
    NAME(FloatSignInject,"FSGNJ.S") NAME(FloatSignInvert,"FSGNJN.S") NAME(FloatSignXor,"FSGNJX.S")
    NAME(FloatMin,"FMIN.S") NAME(FloatMax,"FMAX.S") NAME(FloatLessEqual,"FLE.S") NAME(FloatLess,"FLT.S")
    NAME(FloatEqual,"FEQ.S") NAME(FloatClassify,"FCLASS.S") NAME(FloatToInt,"FCVT.W.S") NAME(FloatToUnsigned,"FCVT.WU.S")
    NAME(IntToFloat,"FCVT.S.W") NAME(UnsignedToFloat,"FCVT.S.WU") NAME(MoveFloatToInt,"FMV.X.W") NAME(MoveIntToFloat,"FMV.W.X")
    NAME(CsrReadWrite,"CSRRW") NAME(CsrSet,"CSRRS") NAME(CsrClear,"CSRRC") NAME(CsrReadWriteImmediate,"CSRRWI")
    NAME(CsrSetImmediate,"CSRRSI") NAME(CsrClearImmediate,"CSRRCI") NAME(Fence,"FENCE") NAME(EnvironmentCall,"ECALL") NAME(Breakpoint,"EBREAK")
    NAME(SfuReciprocal,"gpu.rcp.s") NAME(SfuReciprocalSqrt,"gpu.rsqrt.s") NAME(SfuExp2,"gpu.exp2.s")
    NAME(SfuLog2,"gpu.log2.s") NAME(SfuSin,"gpu.sin.s") NAME(SfuCos,"gpu.cos.s")
    NAME(GraphicsGet,"gfx.get") NAME(GraphicsTexture,"gfx.tex") NAME(GraphicsEnd,"gfx.end")
#undef NAME
    }
    return "UNKNOWN";
}
} // namespace animus
