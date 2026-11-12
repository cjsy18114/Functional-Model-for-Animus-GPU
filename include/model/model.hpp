#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace animus {
using Word = std::uint32_t;
using Addr = std::uint32_t;
using Mask = std::uint32_t;
using WarpId = std::uint32_t;
constexpr unsigned kMaxLanes = 32;
constexpr unsigned kRegisters = 32;
constexpr unsigned kWarpSlots = 4;
constexpr unsigned kStackDepth = 8;
constexpr unsigned kBarriersPerGroup = 8; // Host single-core capacity, not an ISA encoding limit.
constexpr unsigned kBarrierSlots = kWarpSlots * kBarriersPerGroup;

enum class IssueKind { ArchitecturalFault, ModelLimit, Diagnostic, HostError };
struct Issue {
    IssueKind kind;
    std::string code;
    std::string message;
    std::optional<WarpId> warp_id;
    std::optional<unsigned> lane;
    std::optional<Addr> pc;
    std::optional<Word> raw;
    std::optional<Addr> address;
    std::optional<std::string> spec_issue_id;
    std::optional<Word> command_offset{}; // Host diagnostic, not an extra MMIO register.
};
Issue make_issue(IssueKind kind, std::string code, std::string message);

// C++20 has no std::expected: the result contains either a value or an Issue.
template<class T> class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Issue issue) : data_(std::move(issue)) {}
    explicit operator bool() const { return std::holds_alternative<T>(data_); }
    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    const Issue& error() const { return std::get<Issue>(data_); }
private:
    std::variant<T, Issue> data_;
};
template<> class Result<void> {
public:
    Result() = default;
    Result(Issue issue) : issue_(std::move(issue)) {}
    explicit operator bool() const { return !issue_; }
    const Issue& error() const { return issue_.value(); }
private:
    std::optional<Issue> issue_;
};

enum class FpStatePolicy { Unspecified, PerLaneExperimental };
enum class SfuProfile { Disabled, HostMathV1 };
enum class RasterProfile { Disabled, ReferenceV1 };
struct ModelConfig {
    unsigned warp_width = 32;
    unsigned max_warps = 4;
    std::uint64_t memory_bytes = 64ull * 1024 * 1024;
    std::uint64_t max_steps = 1'000'000;
    bool check_undefined = true;
    // Unconfirmed S06/S07 policies: opt in to natural-alignment faults and lane-order stores.
    bool allow_experimental = false;
    // S04/S05 require explicit opt-in; neither silently changes the ISA contract.
    FpStatePolicy fp_state_policy = FpStatePolicy::Unspecified;
    SfuProfile sfu_profile = SfuProfile::Disabled;
    unsigned max_width = 4096, max_height = 4096; // Host resource limits, not ISA texture size guarantees.
    std::uint64_t max_work_items = 10'000'000;
    RasterProfile raster_profile = RasterProfile::Disabled;
};
Result<void> validate_config(const ModelConfig& config);
Mask width_mask(unsigned width); // Precondition: width is in [1,32].

enum class Access : unsigned { Read = 1, Write = 2, Execute = 4 };
constexpr unsigned permission(Access access) { return static_cast<unsigned>(access); }
struct MemoryRegion {
    Addr base;
    std::uint64_t bytes;
    unsigned permissions;
};
struct CodeRegion { Addr base; std::uint64_t bytes; };
class Memory {
public:
    explicit Memory(std::size_t bytes) : bytes_(bytes, 0) {}
    std::size_t size() const { return bytes_.size(); }
    Result<void> map_region(MemoryRegion region);
    Result<void> check_access(Addr address, std::uint64_t bytes, Access access) const;
    Result<Word> load_le(Addr address, unsigned bytes, Access access = Access::Read) const;
    Result<void> store_le(Addr address, unsigned bytes, Word value);
    Result<Word> fetch32(Addr pc) const;
    Result<CodeRegion> executable_region_at(Addr pc) const;
    // Internal helpers: caller must precheck width, full range and write permission.
    Word peek_le_prechecked(Addr address, unsigned bytes) const;
    void store_le_prechecked(Addr address, unsigned bytes, Word value);
    // Loader-only operation: initializes an image before guest execution.
    Result<void> load_image(Addr address, std::span<const std::uint8_t> bytes,
                            unsigned permissions);
private:
    std::vector<std::uint8_t> bytes_;
    std::vector<MemoryRegion> regions_;
};

struct LaneState {
    std::array<Word, kRegisters> x{};
    std::array<Word, kRegisters> f_bits{};
    std::array<bool, kRegisters> x_defined{};
    std::array<bool, kRegisters> f_defined{};
    std::uint8_t fcsr = 0; // [7:5] frm, [4:0] fflags; active only under the explicit S04 policy.
    LaneState() { x_defined[0] = true; }
};

// D-stage host dispatch data. This is not a Command ISA descriptor or a compute ABI.
enum class ShaderStage { Test, Vertex, Fragment };
struct InvocationInput {
    Word vertex_id = 0;
    Addr vertex_addr = 0, uniform_base = 0;
    Word pixel_x = 0, pixel_y = 0, depth_bits = 0, primitive_id = 0;
    Addr prim_data_addr = 0;
    Word bary0_bits = 0, bary1_bits = 0;
};
struct GraphicsState {
    // Keep the D texture prefix for source compatibility; descriptors are decoded
    // explicitly by word offset, never by copying this C++ struct's layout.
    Addr texture_base = 0;
    Word texture_width = 0, texture_height = 0, texture_stride = 0;
    Word version = 1, flags = 0;
    Addr color_base = 0, depth_base = 0;
    Word width = 0, height = 0, color_stride = 0, depth_stride = 0;
    Addr vs_entry = 0, fs_entry = 0, vertex_base = 0;
    Word vertex_stride = 0;
    Addr uniform_base = 0;
    unsigned varying_count = 0;
    std::int32_t viewport_x = 0, viewport_y = 0;
    Word viewport_width = 0, viewport_height = 0;
    std::int32_t scissor_x = 0, scissor_y = 0;
    Word scissor_width = 0, scissor_height = 0;
};
struct DispatchContext {
    ShaderStage stage = ShaderStage::Test;
    unsigned varying_count = 0;
    std::array<InvocationInput, kMaxLanes> inputs{};
    GraphicsState graphics{}; // Value snapshot: later host changes do not change a live dispatch.
};
struct VertexOutput {
    std::uint64_t invocation_id = 0;
    unsigned lane = 0;
    Word vertex_id = 0;
    std::array<Word, 4> position_bits{};
    std::array<Word, 8> varying_bits{};
    unsigned varying_count = 0;
};
struct FragmentOutput {
    std::uint64_t invocation_id = 0;
    unsigned lane = 0;
    InvocationInput input{}; // Original fragment identity, coordinates, depth and interpolation inputs.
    Word rgba8 = 0;
};
struct OutputBatch {
    ShaderStage stage = ShaderStage::Test;
    std::uint64_t invocation_id = 0;
    std::vector<VertexOutput> vertices;
    std::vector<FragmentOutput> fragments;
};
const char* stage_name(ShaderStage stage);
enum class StackPhase { Single, First, Second };
struct ReconvergenceFrame {
    Mask entry_mask = 0, else_mask = 0;
    Addr else_pc = 0, join_pc = 0;
    StackPhase phase = StackPhase::Single;
};
enum class WarpStatus { Ready, Waiting, Ended, Faulted };
enum class WaitReason { None, Spawn, Barrier };
struct WarpState {
    bool allocated = false;
    WarpId id = 0;
    std::array<LaneState, kMaxLanes> lanes{};
    Addr pc = 0;
    Mask init_mask = 0, exec = 0;
    unsigned sp = 0;
    std::array<ReconvergenceFrame, kStackDepth> stack{};
    WarpStatus status = WarpStatus::Ready;
    WaitReason wait_reason = WaitReason::None;
    std::vector<CodeRegion> code_regions{};
    std::uint64_t invocation_id = 0;
    DispatchContext dispatch{};
    std::optional<OutputBatch> completed_outputs; // Owned value copies; valid until explicit slot release.
};

// E-stage host metadata and resumable execution. None of these layouts is guest ABI.
enum class ResourceRole { Command, Descriptor, Shader, Vertex, Uniform, Texture, Color, Depth, Scratch, Completion };
struct Resource {
    ResourceRole role;
    Addr base;
    std::uint64_t bytes;
    unsigned permissions;
};
enum class SubmissionStatus : Word { Idle = 0, Busy = 1, Done = 2, Error = 3 };
struct Command { Word opcode = 0, a = 0, b = 0, c = 0; };
struct RasterVertex { std::array<double,4> position{}; std::array<double,8> varying{}; };
struct Triangle { std::array<RasterVertex,3> vertices{}; Word primitive_id = 0; };
struct RasterTriangle {
    std::array<std::int64_t,3> x{}, y{}; // 1/256-pixel units; normalized to positive edge area.
    std::array<double,3> depth{};
    std::array<Word,3> inv_w{};
    std::array<std::array<Word,3>,8> attr_over_w{};
    std::int64_t area = 0;
    Word primitive_id = 0;
    int left = 0, top = 0, right = -1, bottom = -1;
};
enum class DrawPhase { LaunchVertices, RunVertices, PrepareTriangles, SetupTriangle, Scan, RunFragments, Merge };
struct DrawProgress {
    Word first = 0, count = 0, primitive = 0;
    unsigned vertices_done = 0;
    DrawPhase phase = DrawPhase::LaunchVertices;
    std::optional<WarpId> warp;
    std::vector<VertexOutput> vertices;
    std::vector<Triangle> triangles;
    std::size_t triangle_index = 0;
    std::optional<RasterTriangle> raster;
    Addr interpolation_address = 0;
    std::uint64_t scan_index = 0;
    std::vector<InvocationInput> fragments;
    std::vector<FragmentOutput> outputs;
    std::size_t merge_index = 0;
};
struct SubmissionState {
    Word cmd_base = 0, cmd_bytes = 0, latched_base = 0, latched_bytes = 0, offset = 0;
    SubmissionStatus status = SubmissionStatus::Idle;
    Word error_code = 0, error_offset = 0;
    bool state_valid = false;
    GraphicsState graphics{};
    std::optional<Command> current;
    bool operation_started = false;
    std::uint64_t clear_index = 0, shader_attempts = 0, work_items = 0;
    std::uint64_t completed_commands = 0, completed_primitives = 0, merged_fragments = 0;
    std::optional<DrawProgress> draw;
    std::optional<Issue> last_issue;
};
// Vortex-style blocking local barriers. Each warp contributes one arrival per round.
struct BarrierState {
    Word expected = 0;
    Mask arrived = 0;
    std::uint64_t generation = 0;
};
struct PendingSpawn {
    WarpId caller = 0;
    Word count = 0; // Enable slots [0,count), excluding caller; caller continues at PC+4.
    Addr entry = 0;
};
struct Machine {
    ModelConfig config;
    Memory memory;
    std::array<WarpState, kWarpSlots> warps{};
    std::uint64_t attempted_steps = 0, retired_steps = 0;
    std::uint64_t next_invocation_id = 1;
    std::vector<Resource> resources;
    SubmissionState submission{};
    std::array<BarrierState, kBarrierSlots> barriers{};
    std::optional<PendingSpawn> pending_spawn;
    explicit Machine(const ModelConfig& cfg) : config(cfg), memory(static_cast<std::size_t>(cfg.memory_bytes)) {}
};
Result<Machine> create_machine(const ModelConfig& config = {});
Result<void> install_resources(Machine&, std::span<const Resource>);
Result<void> check_submission_access(const Machine&, Addr, std::uint64_t, Access);
struct LaunchInfo {
    Addr entry = 0;
    Mask init_mask = 0xFFFFFFFFu;
    // Empty defaults to the executable image containing entry; explicit ranges support split images.
    std::vector<CodeRegion> code_regions{};
    DispatchContext dispatch{};
};
Result<std::optional<WarpId>> create_warp(Machine& machine, const LaunchInfo& launch);
// Validate only provided D-stage host data and mapped extents; no inferred driver allocation sizes.
Result<void> validate_dispatch(const Machine&, const LaunchInfo&);
Result<Word> get_input(const DispatchContext&, unsigned lane, std::uint16_t selector);
Result<Word> sample_texture(const Memory&, const GraphicsState&, Word u_bits, Word v_bits);
Result<OutputBatch> collect_outputs(const WarpState&, bool check_undefined = true);
Result<void> initialize_x(Machine& machine, WarpId warp, unsigned lane, unsigned reg, Word value);
// Host initialization: bits is the FP32 encoding, not a numeric integer-to-float conversion; f0 is writable.
Result<void> initialize_f(Machine& machine, WarpId warp, unsigned lane, unsigned reg, Word bits);
// Host test completion only: Ready at stop_pc, empty stack, EXEC == init_mask. Does not execute an ISA exit.
Result<void> finish_test_warp(Machine& machine, WarpId warp, Addr stop_pc);
// Returns a value snapshot (including owned graphics outputs) before clearing a finished/faulted slot.
// gfx.end marks completion but never clears a slot before the host has a chance to retain this copy.
Result<WarpState> release_warp(Machine& machine, WarpId warp);
// Round-robin after a warp slot; excluded uses warp-slot bits (not lane EXEC bits). No state is changed.
std::optional<WarpId> choose_ready(const Machine& machine, std::optional<WarpId> after = {}, Mask excluded = 0);

struct BinaryImage {
    std::filesystem::path path;
    Addr load_address = 0;
    Addr entry = 0;
};
Result<void> load_binary(Machine& machine, const BinaryImage& image);

enum class Op {
    AddImmediate, Add, Subtract, ShiftLeft, LessSigned, LessUnsigned,
    Xor, ShiftRight, ShiftRightArithmetic, Or, And,
    LessSignedImmediate, LessUnsignedImmediate, XorImmediate, OrImmediate, AndImmediate,
    ShiftLeftImmediate, ShiftRightImmediate, ShiftRightArithmeticImmediate, Lui, Auipc,
    LoadByte, LoadHalf, LoadWord, LoadByteUnsigned, LoadHalfUnsigned,
    StoreByte, StoreHalf, StoreWord,
    BranchEqual, BranchNotEqual, BranchLessSigned, BranchGreaterEqualSigned,
    BranchLessUnsigned, BranchGreaterEqualUnsigned, JumpLink, JumpLinkRegister,
    Multiply, MultiplyHighSigned, MultiplyHighSignedUnsigned, MultiplyHighUnsigned,
    DivideSigned, DivideUnsigned, RemainderSigned, RemainderUnsigned,
    Split, Join, ReadThreadMask, WarpSpawn, ThreadMaskControl, Barrier,
    LoadFloat, StoreFloat, FloatAdd, FloatSubtract, FloatMultiply, FloatDivide, FloatSqrt,
    FloatMultiplyAdd, FloatMultiplySubtract, FloatNegMultiplySubtract, FloatNegMultiplyAdd,
    FloatSignInject, FloatSignInvert, FloatSignXor, FloatMin, FloatMax,
    FloatLessEqual, FloatLess, FloatEqual, FloatClassify,
    FloatToInt, FloatToUnsigned, IntToFloat, UnsignedToFloat, MoveFloatToInt, MoveIntToFloat,
    CsrReadWrite, CsrSet, CsrClear, CsrReadWriteImmediate, CsrSetImmediate, CsrClearImmediate,
    Fence, EnvironmentCall, Breakpoint,
    SfuReciprocal, SfuReciprocalSqrt, SfuExp2, SfuLog2, SfuSin, SfuCos,
    GraphicsGet, GraphicsTexture, GraphicsEnd
};
enum class InstructionClass { Alu, MulDiv, LoadStore, Control, Simt, ThreadMask, Float, Csr, Environment, Sfu, Graphics };
struct DecodedInst {
    Word raw = 0;
    Op op = Op::AddImmediate;
    unsigned rd = 0, rs1 = 0, rs2 = 0;
    Word immediate_bits = 0; // Sign-extended immediate/byte offset, U bits, or unsigned shift amount.
    InstructionClass instruction_class = InstructionClass::Alu;
    bool uses_rs1 = false, uses_rs2 = false;
    unsigned csr_operation = 0; // Zicsr funct3: 1/2/3 register source, 5/6/7 immediate source.
    unsigned rs3 = 0, rm = 0;
    std::uint16_t csr = 0;
    bool uses_fs1 = false, uses_fs2 = false, uses_fs3 = false;
    std::uint16_t selector = 0; // Unsigned 12-bit graphics input selector.
};
Result<DecodedInst> decode(Word raw);
const char* op_name(Op op);
enum class RegisterFile { Integer, Float };
struct RegisterWrite { unsigned lane, reg; Word before, after; RegisterFile file = RegisterFile::Integer; };
struct CsrWrite { unsigned lane; std::uint16_t csr; Word before, after; }; // Full fcsr snapshots, including alias accesses.
struct MemoryWrite { unsigned lane; Addr address; unsigned bytes; Word before, after; };
struct ControlUpdate {
    Mask exec;
    unsigned sp;
    std::array<ReconvergenceFrame, kStackDepth> stack;
};
// Only management instructions take a control-plane snapshot. Vector keeps the
// four large register files off the stack; evaluate remains side-effect free.
struct ManagementEffect {
    std::vector<WarpState> warps;
    std::array<BarrierState, kBarrierSlots> barriers{};
    std::optional<PendingSpawn> pending_spawn;
    std::uint64_t next_invocation_id = 0;
};
struct StepEffect {
    Addr next_pc = 0;
    std::vector<RegisterWrite> register_writes;
    std::vector<MemoryWrite> memory_writes;
    std::optional<ControlUpdate> control;
    std::vector<CsrWrite> csr_writes;
    std::optional<OutputBatch> outputs;
    std::optional<WarpStatus> next_status;
    std::optional<ManagementEffect> management;
};
struct WarpEvent {
    WarpId warp;
    std::uint64_t invocation_id;
    Addr pc;
    Mask exec;
    WarpStatus status;
    WaitReason wait_reason;
};
struct StepRecord {
    std::uint64_t step = 0;
    WarpId warp_id = 0;
    Addr pc_before = 0, pc_after = 0;
    Mask exec_before = 0, exec_after = 0;
    unsigned sp_before = 0, sp_after = 0;
    Word raw = 0;
    Op op = Op::AddImmediate;
    std::vector<RegisterWrite> register_writes;
    std::vector<MemoryWrite> memory_writes;
    bool experimental_profile = false;
    std::vector<CsrWrite> csr_writes;
    FpStatePolicy fp_state_policy = FpStatePolicy::Unspecified;
    SfuProfile sfu_profile = SfuProfile::Disabled;
    std::uint64_t invocation_id = 0;
    std::optional<OutputBatch> outputs;
    WarpStatus status_before = WarpStatus::Ready, status_after = WarpStatus::Ready;
    std::optional<Word> command_offset;
    RasterProfile raster_profile = RasterProfile::Disabled;
    std::vector<WarpEvent> warp_events; // Spawn/wait/wakeup/exit, including other affected warps.
};
Result<StepEffect> evaluate(const Machine& machine, WarpId warp, const DecodedInst& inst);
// Internal commit boundary: effect must come from successful evaluate of the current state.
void commit_effect(Machine& machine, WarpId warp, const StepEffect& effect);
Result<StepRecord> step_shader(Machine& machine, WarpId warp);
enum class StopReason { Paused, Timeout, ArchitecturalFault, ModelLimit, Diagnostic, HostError, SuspectedDeadlock, Completed };
struct RunReport {
    StopReason reason;
    std::uint64_t retired_steps;
    std::optional<Issue> issue;
};
// Single selected warp only. Reaching stop_pc is a debugger pause, not shader termination.
RunReport run_until_stop(Machine& machine, WarpId warp, Addr stop_pc, std::ostream* trace = nullptr);
struct WarpStop { WarpId warp; Addr stop_pc; };
RunReport run_warps_until_stops(Machine& machine, std::span<const WarpStop> stops, std::ostream* trace = nullptr);
// Selected VS/FS warps only; return Completed after each actually executes gfx.end. Does not release slots.
RunReport run_shader_batch(Machine&, std::span<const WarpId> warps, std::ostream* trace = nullptr);
// Run every live Test-stage warp, including newly spawned children, until tmc 0.
// Graphics submissions use their existing runner. Budgets/deadlocks preserve state.
RunReport run_management(Machine&, std::ostream* trace = nullptr);
void write_trace(std::ostream& output, const StepRecord& record);
void write_issue(std::ostream& output, const Issue& issue);
} // namespace animus
