#include "model/model.hpp"
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace animus;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> void ok(const Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}
Machine small_machine() {
    ModelConfig config;
    config.memory_bytes = 256;
    auto result = create_machine(config);
    ok(result);
    return std::move(result.value());
}
void install(Machine& machine, std::initializer_list<Word> words) {
    std::vector<std::uint8_t> bytes;
    for (Word word : words)
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(word >> (8 * i)));
    ok(machine.memory.load_image(0, bytes, permission(Access::Read) | permission(Access::Execute)));
}
WarpId launch(Machine& machine, Mask mask = 0xFFFFFFFFu) {
    auto result = create_warp(machine, {0, mask}); ok(result);
    require(result.value().has_value(), "Expected nonempty launch");
    return result.value().value();
}
void config_and_state() {
    ModelConfig config;
    require(config.warp_width == 32 && config.max_warps == 4, "Target configuration");
    require(width_mask(32) == 0xFFFFFFFFu && width_mask(1) == 1, "Width masks");
    config.warp_width = 0; require(!validate_config(config), "Reject zero lanes");
    config.warp_width = 33; require(!validate_config(config), "Reject 33 lanes");
    config.warp_width = 32; config.max_warps = 5; require(!validate_config(config), "Reject five slots");
    config.max_warps = 4; config.memory_bytes = 0; require(!create_machine(config), "Reject empty memory");
    config.memory_bytes = (1ull << 32) + 1; require(!validate_config(config), "Reject oversized memory");
    config.memory_bytes = 256; config.max_steps = 0; require(!validate_config(config), "Reject zero budget");
    auto machine = small_machine();
    auto empty = create_warp(machine, {0, 0}); ok(empty);
    require(!empty.value(), "Empty launch needs no executable mapping");
    install(machine, {0x00100093u});
    for (unsigned i = 0; i < 4; ++i) {
        const auto id = launch(machine);
        require(id == i && machine.warps[id].pc == 0 && machine.warps[id].sp == 0, "Slot initialization");
        require(machine.warps[id].exec == 0xFFFFFFFFu, "All lanes active");
        require(machine.warps[id].lanes[31].x[0] == 0, "Lane 31 exists");
    }
    auto fifth = create_warp(machine, {0, 1});
    require(!fifth && fifth.error().code == "NoFreeWarpSlot", "Reject fifth allocated warp");
    require(!initialize_x(machine, 4, 0, 1, 1), "Reject invalid warp register initialization");
}
void memory_access() {
    Memory memory(16);
    ok(memory.map_region({0, 8, permission(Access::Read) | permission(Access::Write)}));
    ok(memory.map_region({8, 8, permission(Access::Read)}));
    ok(memory.store_le(0, 4, 0x12345678u));
    require(memory.load_le(0, 1).value() == 0x78, "Little-endian byte");
    require(memory.load_le(1, 2).value() == 0x3456, "Byte helper supports unaligned access");
    require(memory.load_le(0, 4).value() == 0x12345678u, "Word round trip");
    ok(memory.store_le(6, 2, 0xABCD));
    require(memory.load_le(6, 2).value() == 0xABCD, "Last writable halfword");
    require(!memory.store_le(7, 2, 0), "Full range must fit mapping");
    require(memory.load_le(7, 1).value() == 0xAB, "Rejected write does not partially modify memory");
    require(!memory.store_le(8, 1, 1), "Read-only protection");
    require(!memory.fetch32(0), "Execute permission required");
    require(!memory.load_le(15, 2), "End boundary");
    require(!memory.check_access(0xFFFFFFFFu, 4, Access::Read), "Address wrap rejected");
    require(!memory.check_access(0, std::numeric_limits<std::uint64_t>::max(), Access::Read), "Huge range rejected");
    require(!memory.load_le(0, 3), "Unsupported width");
    require(!memory.map_region({4, 4, 1}), "Overlap rejected");
    require(!memory.map_region({16, 1, 1}), "Mapping outside memory rejected");
}
void decode_vectors() {
    // Independent known machine words; no shared test encoder hides bit-field errors.
    auto addi = decode(0xFFF00093u); ok(addi); // addi x1,x0,-1
    require(addi.value().rd == 1 && addi.value().rs1 == 0 && addi.value().rs2 == 0, "ADDI fields");
    require(addi.value().immediate_bits == 0xFFFFFFFFu, "Negative immediate");
    require(decode(0x80000093u).value().immediate_bits == 0xFFFFF800u, "Minimum immediate");
    require(decode(0x7FF00093u).value().immediate_bits == 2047, "Maximum immediate");
    auto add = decode(0x00108133u); ok(add); // add x2,x1,x1
    require(add.value().op == Op::Add && add.value().rd == 2 && add.value().rs1 == 1 && add.value().rs2 == 1, "ADD fields");
    require(!decode(0) && decode(0).error().kind == IssueKind::ArchitecturalFault, "Invalid word");
    require(decode(0x40108133u).value().op == Op::Subtract, "SUB is distinct from ADD");
    require(decode(0x00101093u).value().op == Op::ShiftLeftImmediate, "Shift encoding is distinct from ADDI");
    require(decode(0x00100073u).value().op == Op::Breakpoint, "EBREAK decodes as an exception operation, not NOP");
}
void execution_and_trace() {
    auto machine = small_machine();
    install(machine, {0xFFF00093u, 0x00108133u, 0x00110113u, 0x00108033u});
    auto id = launch(machine);
    auto first = step_shader(machine, id); ok(first);
    require(machine.warps[id].pc == 4 && first.value().register_writes.size() == 32, "Single warp PC advance");
    require(machine.warps[id].lanes[31].x[1] == 0xFFFFFFFFu, "Lane31 negative result");
    ok(step_shader(machine, id));
    require(machine.warps[id].lanes[0].x[2] == 0xFFFFFFFEu, "Unsigned wraparound ADD");
    ok(step_shader(machine, id)); // addi x2,x2,1: source/destination alias
    require(machine.warps[id].lanes[31].x[2] == 0xFFFFFFFFu, "Aliased operand reads old state");
    auto zero = step_shader(machine, id); ok(zero);
    require(zero.value().register_writes.empty() && machine.warps[id].lanes[0].x[0] == 0, "x0 writes discarded");
    std::ostringstream trace; write_trace(trace, first.value());
    require(trace.str().find("\"op\":\"ADDI\"") != std::string::npos && trace.str().find("\"lane\":31") != std::string::npos, "Trace includes opcode and lane31");
    require(machine.retired_steps == 4 && machine.attempted_steps == 4, "Counters");
}
void masked_and_prechecked() {
    auto machine = small_machine();
    install(machine, {0x00108093u}); // addi x1,x1,1
    auto id = launch(machine, 0x80000000u);
    ok(initialize_x(machine, id, 31, 1, 9));
    ok(step_shader(machine, id));
    require(machine.warps[id].lanes[31].x[1] == 10 && !machine.warps[id].lanes[0].x_defined[1], "Inactive lanes not read/written");
    auto other = launch(machine);
    for (unsigned lane = 0; lane < 31; ++lane) ok(initialize_x(machine, other, lane, 1, 7));
    auto failed = step_shader(machine, other);
    require(!failed && failed.error().code == "UndefinedRegister" && failed.error().lane == 31, "Late-lane diagnostic");
    require(machine.warps[other].pc == 0 && machine.warps[other].lanes[0].x[1] == 7, "No partial commit");
    require(machine.warps[other].status == WarpStatus::Ready, "Diagnostic does not fake hardware fault");
    auto prepared = decode(0x00108093u); ok(prepared);
    ok(initialize_x(machine, other, 31, 1, 7));
    auto effect = evaluate(machine, other, prepared.value()); ok(effect);
    require(machine.warps[other].lanes[0].x[1] == 7, "Evaluate is read-only");
    commit_effect(machine, other, effect.value());
    require(machine.warps[other].lanes[0].x[1] == 8, "Commit applies effect");
}
void failure_context() {
    auto machine = small_machine();
    install(machine, {0}); auto id = launch(machine);
    auto result = step_shader(machine, id);
    require(!result && result.error().pc == 0 && result.error().raw == 0 && result.error().warp_id == id, "Fault context");
    require(machine.warps[id].status == WarpStatus::Faulted && machine.retired_steps == 0, "Fault doesn't retire");
    auto budget = small_machine(); budget.config.max_steps = 1;
    install(budget, {0x00100093u, 0x00108133u}); auto bid = launch(budget);
    ok(step_shader(budget, bid)); auto exhausted = step_shader(budget, bid);
    require(!exhausted && exhausted.error().code == "StepBudget" && budget.warps[bid].pc == 4, "Budget is host stop");
    auto alignment = small_machine(); install(alignment, {0x00100093u});
    require(!create_warp(alignment, {2, 1}), "Entry alignment");
    require(!step_shader(alignment, 4), "Invalid step target");
}
void binary_loader() {
    const auto path = std::filesystem::current_path() / "stage_a_loader_test.bin";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    { std::ofstream output(path, std::ios::binary); const char bytes[] = {char(0x93), 0, char(0x50), 0}; output.write(bytes, 4); }
    auto machine = small_machine(); ok(load_binary(machine, {path, 16, 16}));
    require(machine.memory.fetch32(16).value() == 0x00500093u, "File bytes and load base");
    require(!machine.memory.store_le(16, 4, 0), "Code is not writable");
    require(!load_binary(machine, {path, 32, 36}), "Entry outside code");
    { std::ofstream output(path, std::ios::binary | std::ios::trunc); output.put('x'); }
    require(!load_binary(machine, {path, 32, 32}), "Truncated code word rejected");
    require(!load_binary(machine, {path.string() + ".missing", 32, 32}), "Missing image diagnosed");
}
}
int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {{"A1 config and state", config_and_state}, {"A2 memory", memory_access},
        {"A3 decode", decode_vectors}, {"A4 execution and trace", execution_and_trace},
        {"mask and atomic precheck", masked_and_prechecked}, {"failure context and budget", failure_context},
        {"binary loader", binary_loader}};
    unsigned failures = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
