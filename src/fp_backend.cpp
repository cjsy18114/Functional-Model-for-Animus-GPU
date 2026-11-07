#include "model/execution.hpp"
#include <bit>
#include <cstdint>
#include <cfenv>
#include <cmath>
#include <limits>
extern "C" {
#include "softfloat.h"
}

namespace animus {
static_assert(std::endian::native == std::endian::little, "Vendored SoftFloat platform requires little endian.");
namespace {
constexpr Word kCanonicalNaN = 0x7FC00000u;
bool nan(Word value) { return (value & 0x7FFFFFFFu) > 0x7F800000u; }
bool signaling_nan(Word value) { return nan(value) && (value & 0x00400000u) == 0; }
// SoftFloat state is process-global. Save/restore it per operation; guest CSR state never lives here.
// The model is single-threaded; parallel host callers would require an external lock or TLS build.
class SoftFloatScope {
    uint_fast8_t round_, flags_, tiny_;
public:
    explicit SoftFloatScope(unsigned rounding) : round_(softfloat_roundingMode), flags_(softfloat_exceptionFlags), tiny_(softfloat_detectTininess) {
        softfloat_roundingMode = static_cast<uint_fast8_t>(rounding);
        softfloat_exceptionFlags = 0;
        softfloat_detectTininess = softfloat_tininess_afterRounding;
    }
    ~SoftFloatScope() { softfloat_roundingMode = round_; softfloat_exceptionFlags = flags_; softfloat_detectTininess = tiny_; }
};
Word classify(Word value) {
    const bool sign = (value >> 31) != 0;
    const Word exponent = (value >> 23) & 255u, fraction = value & 0x7FFFFFu;
    if (nan(value)) return Word{1} << (signaling_nan(value) ? 8 : 9);
    if (exponent == 255) return Word{1} << (sign ? 0 : 7);
    if (exponent == 0) return Word{1} << (fraction == 0 ? (sign ? 3 : 4) : (sign ? 2 : 5));
    return Word{1} << (sign ? 1 : 6);
}
Word min_max(Word a, Word b, bool maximum) {
    if (signaling_nan(a) || signaling_nan(b)) softfloat_raiseFlags(softfloat_flag_invalid);
    if (nan(a) && nan(b)) return kCanonicalNaN;
    if (nan(a)) return b;
    if (nan(b)) return a;
    if ((a & 0x7FFFFFFFu) == 0 && (b & 0x7FFFFFFFu) == 0) return maximum ? (a & b) : (a | b);
    const bool less = f32_lt_quiet(float32_t{a}, float32_t{b});
    return maximum ? (less ? b : a) : (less ? a : b);
}
Word quantize_double(double value) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    SoftFloatScope scope(0);
    const Word bits = f64_to_f32(float64_t{std::bit_cast<std::uint64_t>(value)}).v;
    return nan(bits) ? kCanonicalNaN : bits;
}
}
Result<unsigned> resolve_rm(unsigned rm, unsigned frm) {
    const unsigned resolved = rm == 7 ? frm : rm;
    if (resolved > 4) return make_issue(IssueKind::ArchitecturalFault, "InvalidRoundingMode", "Reserved floating-point rounding mode.");
    return resolved;
}
Result<FpResult> fp_eval(Op op, Word a, Word b, Word c, unsigned rounding_mode) {
    if (rounding_mode > 4) return make_issue(IssueKind::HostError, "InvalidRoundingMode", "Backend requires a resolved rounding mode 0..4.");
    static_assert(softfloat_flag_invalid == 16 && softfloat_flag_infinite == 8 && softfloat_flag_inexact == 1);
    SoftFloatScope scope(rounding_mode);
    const float32_t fa{a}, fb{b}, fc{c};
    Word result = 0;
    bool float_result = true;
    switch (op) {
    case Op::FloatAdd: result = f32_add(fa, fb).v; break;
    case Op::FloatSubtract: result = f32_sub(fa, fb).v; break;
    case Op::FloatMultiply: result = f32_mul(fa, fb).v; break;
    case Op::FloatDivide: result = f32_div(fa, fb).v; break;
    case Op::FloatSqrt: result = f32_sqrt(fa).v; break;
    case Op::FloatMultiplyAdd: result = f32_mulAdd(fa, fb, fc).v; break;
    case Op::FloatMultiplySubtract: result = f32_mulAdd(fa, fb, float32_t{c ^ 0x80000000u}).v; break;
    case Op::FloatNegMultiplySubtract: result = f32_mulAdd(float32_t{a ^ 0x80000000u}, fb, fc).v; break;
    case Op::FloatNegMultiplyAdd: result = f32_mulAdd(float32_t{a ^ 0x80000000u}, fb, float32_t{c ^ 0x80000000u}).v; break;
    case Op::FloatSignInject: return FpResult{(a & 0x7FFFFFFFu) | (b & 0x80000000u), 0};
    case Op::FloatSignInvert: return FpResult{(a & 0x7FFFFFFFu) | ((~b) & 0x80000000u), 0};
    case Op::FloatSignXor: return FpResult{a ^ (b & 0x80000000u), 0};
    case Op::FloatMin: result = min_max(a, b, false); break;
    case Op::FloatMax: result = min_max(a, b, true); break;
    case Op::FloatLessEqual: result = f32_le(fa, fb); float_result = false; break;
    case Op::FloatLess: result = f32_lt(fa, fb); float_result = false; break;
    case Op::FloatEqual: result = f32_eq(fa, fb); float_result = false; break;
    case Op::FloatClassify: return FpResult{classify(a), 0};
    case Op::FloatToInt:
        result = static_cast<Word>(f32_to_i32(fa, static_cast<uint_fast8_t>(rounding_mode), true));
        if (softfloat_exceptionFlags & softfloat_flag_invalid) result = nan(a) || !(a >> 31) ? 0x7FFFFFFFu : 0x80000000u;
        float_result = false; break;
    case Op::FloatToUnsigned:
        result = static_cast<Word>(f32_to_ui32(fa, static_cast<uint_fast8_t>(rounding_mode), true));
        if (softfloat_exceptionFlags & softfloat_flag_invalid) result = nan(a) || !(a >> 31) ? 0xFFFFFFFFu : 0;
        float_result = false; break;
    case Op::IntToFloat: result = i32_to_f32(std::bit_cast<std::int32_t>(a)).v; break;
    case Op::UnsignedToFloat: result = ui32_to_f32(a).v; break;
    case Op::MoveFloatToInt: case Op::MoveIntToFloat: return FpResult{a, 0};
    default: return make_issue(IssueKind::HostError, "InvalidDecodedOp", "Not a standard FP operation.");
    }
    // RISC-V specifies a canonical NaN for arithmetic, unlike payload-propagating x86 specialization.
    if (float_result && nan(result)) result = kCanonicalNaN;
    return FpResult{result, static_cast<std::uint8_t>(softfloat_exceptionFlags)};
}
Result<Word> sfu_eval(Op op, Word input, SfuProfile profile) {
    if (profile != SfuProfile::HostMathV1) {
        auto issue = make_issue(IssueKind::ModelLimit, "SfuProfileRequired", "S05: select the explicit HostMathV1 reference profile before running SFU.");
        issue.spec_issue_id = "S05"; return issue;
    }
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    // HostMathV1: radians; f64 libm then RNE FP32; canonical NaN; no guest fflags updates.
    // This is a functional reference with platform-dependent libm error, not a hardware accuracy promise.
    std::fenv_t saved;
    if (std::fegetenv(&saved) != 0) return make_issue(IssueKind::HostError, "HostFloatEnvironment", "Cannot save host FP environment.");
    struct Restore { const std::fenv_t& environment; ~Restore() { std::fesetenv(&environment); } } restore{saved};
    if (std::fesetround(FE_TONEAREST) != 0) return make_issue(IssueKind::HostError, "HostFloatEnvironment", "Cannot set host rounding mode.");
    if (nan(input)) return kCanonicalNaN;
    const double x = static_cast<double>(std::bit_cast<float>(input));
    double value = 0;
    switch (op) {
    case Op::SfuReciprocal: value = 1.0 / x; break;
    case Op::SfuReciprocalSqrt: value = 1.0 / std::sqrt(x); break;
    case Op::SfuExp2: value = std::exp2(x); break;
    case Op::SfuLog2: value = std::log2(x); break;
    case Op::SfuSin: value = std::sin(x); break;
    case Op::SfuCos: value = std::cos(x); break;
    default: return make_issue(IssueKind::HostError, "InvalidDecodedOp", "Not an SFU operation.");
    }
    return quantize_double(value);
}
} // namespace animus
