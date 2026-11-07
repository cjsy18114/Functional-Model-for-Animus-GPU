#pragma once
#include "model/model.hpp"
#include <cstdint>

// Internal execution helpers shared by instruction modules. evaluate() validates sources first.
namespace animus {
Word sign_extend_bits(Word value, unsigned width);
std::int64_t signed_value(Word value); // Exact [-2^31,2^31-1] value, without signed overflow.
Word read_x(const WarpState& warp, unsigned lane, unsigned reg);
void append_x_write(StepEffect&, const WarpState&, unsigned lane, unsigned reg, Word value);
Result<void> check_target(const Machine&, const WarpState&, Addr target);
Result<void> check_target_alignment(Addr target);
Result<StepEffect> eval_alu(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_muldiv(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_load_store(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_control(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_simt(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_thread_mask(const Machine&, WarpId, const DecodedInst&);
void append_f_write(StepEffect&, const WarpState&, unsigned lane, unsigned reg, Word bits);
Result<StepEffect> eval_fp(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_sfu(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_csr(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_environment(const Machine&, WarpId, const DecodedInst&);
Result<StepEffect> eval_graphics(const Machine&, WarpId, const DecodedInst&);
struct FpResult { Word bits; std::uint8_t flags; };
Result<unsigned> resolve_rm(unsigned rm, unsigned frm);
Result<FpResult> fp_eval(Op, Word a, Word b, Word c, unsigned rounding_mode);
Result<Word> sfu_eval(Op, Word input, SfuProfile);
Result<Word> read_csr(const Machine&, WarpId, unsigned lane, std::uint16_t address);
} // namespace animus
