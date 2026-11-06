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

enum class IssueKind { ArchitecturalFault, ModelLimit, Diagnostic, HostError };
struct Issue {
    IssueKind kind;
    std::string code;
    std::string message;
    std::optional<WarpId> warp_id;
    std::optional<unsigned> lane;
    std::optional<Addr> pc;
    std::optional<Word> raw;
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

struct ModelConfig {
    unsigned warp_width = 32;
    unsigned max_warps = 4;
    std::uint64_t memory_bytes = 64ull * 1024 * 1024;
    std::uint64_t max_steps = 1'000'000;
    bool check_undefined = true;
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
class Memory {
public:
    explicit Memory(std::size_t bytes) : bytes_(bytes, 0) {}
    std::size_t size() const { return bytes_.size(); }
    Result<void> map_region(MemoryRegion region);
    Result<void> check_access(Addr address, std::uint64_t bytes, Access access) const;
    Result<Word> load_le(Addr address, unsigned bytes, Access access = Access::Read) const;
    Result<void> store_le(Addr address, unsigned bytes, Word value);
    Result<Word> fetch32(Addr pc) const;
    // Loader-only operation: initializes an image before guest execution.
    Result<void> load_image(Addr address, std::span<const std::uint8_t> bytes,
                            unsigned permissions);
private:
    std::vector<std::uint8_t> bytes_;
    std::vector<MemoryRegion> regions_;
};

struct LaneState {
    std::array<Word, kRegisters> x{};
    std::array<Word, kRegisters> f_bits{}; // Reserved state; F instructions are not implemented.
    std::array<bool, kRegisters> x_defined{};
    LaneState() { x_defined[0] = true; }
};
enum class StackPhase { Single, First, Second };
struct ReconvergenceFrame {
    Mask entry_mask = 0, else_mask = 0;
    Addr else_pc = 0, join_pc = 0;
    StackPhase phase = StackPhase::Single;
};
enum class WarpStatus { Ready, Waiting, Ended, Faulted };
struct WarpState {
    bool allocated = false;
    WarpId id = 0;
    std::array<LaneState, kMaxLanes> lanes{};
    Addr pc = 0;
    Mask init_mask = 0, exec = 0;
    unsigned sp = 0;
    std::array<ReconvergenceFrame, kStackDepth> stack{};
    WarpStatus status = WarpStatus::Ready;
};
struct Machine {
    ModelConfig config;
    Memory memory;
    std::array<WarpState, kWarpSlots> warps{};
    std::uint64_t attempted_steps = 0, retired_steps = 0;
    explicit Machine(const ModelConfig& cfg) : config(cfg), memory(static_cast<std::size_t>(cfg.memory_bytes)) {}
};
Result<Machine> create_machine(const ModelConfig& config = {});
struct LaunchInfo { Addr entry = 0; Mask init_mask = 0xFFFFFFFFu; };
Result<std::optional<WarpId>> create_warp(Machine& machine, const LaunchInfo& launch);
Result<void> initialize_x(Machine& machine, WarpId warp, unsigned lane, unsigned reg, Word value);

struct BinaryImage {
    std::filesystem::path path;
    Addr load_address = 0;
    Addr entry = 0;
};
Result<void> load_binary(Machine& machine, const BinaryImage& image);

enum class Op { AddImmediate, Add };
struct DecodedInst {
    Word raw = 0;
    Op op = Op::AddImmediate;
    unsigned rd = 0, rs1 = 0, rs2 = 0;
    Word immediate_bits = 0; // Sign-extended 12-bit immediate, represented modulo 2^32.
};
Result<DecodedInst> decode(Word raw);
const char* op_name(Op op);
struct RegisterWrite { unsigned lane, reg; Word before, after; };
struct StepEffect {
    Addr next_pc = 0;
    std::vector<RegisterWrite> register_writes;
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
};
Result<StepEffect> evaluate(const Machine& machine, WarpId warp, const DecodedInst& inst);
// Internal commit boundary: effect must come from successful evaluate of the current state.
void commit_effect(Machine& machine, WarpId warp, const StepEffect& effect);
Result<StepRecord> step_shader(Machine& machine, WarpId warp);
void write_trace(std::ostream& output, const StepRecord& record);
void write_issue(std::ostream& output, const Issue& issue);
} // namespace animus
