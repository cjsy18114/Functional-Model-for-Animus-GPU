#include "model/model.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace animus;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> void ok(const Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}
Word addi(unsigned rd, unsigned rs, int value) { return ((Word(value) & 4095u) << 20) | (rs << 15) | (rd << 7) | 0x13u; }
Word ctl(unsigned function, unsigned a = 1, unsigned b = 2) {
    return (function << 25) | (b << 20) | (a << 15) | 0x0Bu;
}
constexpr Word exit_inst = 0x0200000Bu; // Animus funct7=1, rs1=x0, rs2=x0.
Machine make(const std::vector<Word>& code, Mask mask = 0xFFFFFFFFu, unsigned width = 32) {
    ModelConfig config; config.memory_bytes = 8192; config.warp_width = width;
    auto created = create_machine(config); ok(created); auto machine = std::move(created.value());
    std::vector<std::uint8_t> bytes(512, 0);
    for (std::size_t word = 0; word < code.size(); ++word)
        for (unsigned byte = 0; byte < 4; ++byte) bytes.at(4 * word + byte) = std::uint8_t(code[word] >> (8 * byte));
    ok(machine.memory.load_image(0, bytes, 5));
    ok(machine.memory.map_region({1024, 1024, 3}));
    auto launched = create_warp(machine, {0, mask}); ok(launched);
    return machine;
}
WarpId launch(Machine& machine, Addr entry = 0, Mask mask = 0xFFFFFFFFu) {
    auto result = create_warp(machine, {entry, mask}); ok(result); return result.value().value();
}
void x(Machine& machine, WarpId warp, unsigned lane, unsigned reg, Word value) {
    ok(initialize_x(machine, warp, lane, reg, value));
}
void args(Machine& machine, WarpId warp, Word first, Word second, unsigned lane = 31) {
    x(machine, warp, lane, 1, first); x(machine, warp, lane, 2, second);
}
StepRecord step(Machine& machine, WarpId warp = 0) {
    auto result = step_shader(machine, warp); ok(result); return result.value();
}
void issue(Machine& machine, const char* expected, WarpId warp = 0) {
    const auto before = machine.warps; const auto retired = machine.retired_steps;
    const auto barriers = machine.barriers; const auto serial = machine.next_invocation_id;
    auto result = step_shader(machine, warp);
    require(!result && result.error().code == expected, expected);
    require(machine.retired_steps == retired && machine.next_invocation_id == serial, "Failure must not retire/allocate");
    for (unsigned id = 0; id < kWarpSlots; ++id) {
        require(machine.warps[id].pc == before[id].pc && machine.warps[id].exec == before[id].exec &&
                machine.warps[id].invocation_id == before[id].invocation_id, "No partial warp update");
        require(machine.warps[id].lanes[0].x == before[id].lanes[0].x, "No partial register write");
    }
    for (unsigned i = 0; i < kBarrierSlots; ++i)
        require(machine.barriers[i].arrived == barriers[i].arrived && machine.barriers[i].generation == barriers[i].generation,
                "No partial barrier arrival");
}
void decoder() {
    for (const auto& [raw, op] : std::array<std::pair<Word, Op>, 3>{{
        {ctl(0), Op::WarpSpawn}, {ctl(1, 1, 0), Op::ThreadMaskControl}, {ctl(4), Op::Barrier}}}) {
        auto result = decode(raw); ok(result); require(result.value().op == op, "Decode Animus encoding");
        require(!decode(raw | (1u << 7)), "rd must be zero");
        require(!decode(raw | (1u << 12)), "funct3 must be zero");
    }
    require(!decode(ctl(1, 1, 2)), "TMC rs2 must be zero");
}
void tmc_masks() {
    auto machine = make({ctl(1, 1, 0), addi(3, 0, 7), ctl(1, 2, 0), exit_inst});
    // Highest active lane is 31, not lane 0. Other argument lanes are undefined.
    x(machine, 0, 31, 1, 5); step(machine);
    require(machine.warps[0].exec == 5, "TMC exact bit mask, not a lane count");
    step(machine);
    require(machine.warps[0].lanes[0].x[3] == 7 && machine.warps[0].lanes[2].x[3] == 7 &&
            !machine.warps[0].lanes[1].x_defined[3], "Inactive lanes untouched");
    x(machine, 0, 2, 2, 0xFFFFFFFFu); step(machine);
    require(machine.warps[0].exec == 0xFFFFFFFFu, "Re-enable lanes within original envelope");
    auto last = step(machine);
    require(machine.warps[0].exec == 0 && machine.warps[0].status == WarpStatus::Ended &&
            last.pc_after == 16 && machine.retired_steps == 4, "TMC zero ends and retires once");
    auto snapshot = release_warp(machine, 0); ok(snapshot); require(snapshot.value().exec == 0, "Collect ended management warp");
    auto narrow = make({ctl(1, 1, 0)}, 15, 4); x(narrow, 0, 3, 1, 0x80000005u); step(narrow);
    require(narrow.warps[0].exec == 5, "Ignore bits above configured width");
}
void tmc_guards() {
    auto split = make({ctl(1, 1, 0)}); x(split, 0, 31, 1, 0); split.warps[0].sp = 1;
    issue(split, "ManagementInsideSplit");
    auto shader = make({exit_inst}); shader.warps[0].dispatch.stage = ShaderStage::Vertex;
    issue(shader, "ManagementDuringShader");
    auto other = make({exit_inst}); auto id = launch(other); other.warps[id].dispatch.stage = ShaderStage::Fragment;
    issue(other, "ManagementDuringShader");
    auto limited = make({ctl(1, 1, 0)}, 1); x(limited, 0, 0, 1, 3); issue(limited, "MaskOutsideLaunch");
    auto undefined = make({ctl(1, 1, 0)}); issue(undefined, "UndefinedRegister");
}
void spawn_initialization() {
    auto machine = make({ctl(0)}); args(machine, 0, 4, 64);
    x(machine, 0, 0, 7, 0xCAFEBABEu);
    const auto retired = machine.retired_steps;
    auto decoded = decode(ctl(0)); ok(decoded); auto effect = evaluate(machine, 0, decoded.value()); ok(effect);
    require(!machine.warps[1].allocated && machine.retired_steps == retired, "Evaluate WSPAWN is pure");
    auto record = step(machine);
    require(record.warp_events.size() == 4 && machine.warps[0].pc == 4, "Parent continues, children tracked");
    for (unsigned id = 1; id < 4; ++id) {
        const auto& child = machine.warps[id];
        require(child.allocated && child.id == id && child.pc == 64 && child.exec == 1 && child.init_mask == 0xFFFFFFFFu,
                "Child starts at entry with lane zero only");
        require(child.sp == 0 && child.lanes[0].fcsr == 0 && child.lanes[0].x_defined[0] &&
                !child.lanes[0].x_defined[7] && !child.lanes[0].f_defined[0], "Fresh child, no register inheritance");
        require(child.invocation_id != machine.warps[0].invocation_id && child.code_regions[0].base == 0,
                "Fresh identity and inherited code bounds");
    }
}
void spawn_counts() {
    for (Word count : {0u, 1u, 2u, 4u, 99u}) {
        auto machine = make({ctl(0)}); args(machine, 0, count, 64); step(machine);
        unsigned allocated = 0; for (const auto& warp : machine.warps) if (warp.allocated) ++allocated;
        const unsigned expected = count == 0 ? 1 : count > 4 ? 4 : count;
        require(allocated == expected && !machine.pending_spawn, "Count includes root and is clamped to capacity");
    }
    auto no_target = make({ctl(0)}); args(no_target, 0, 1, 3); step(no_target); // No child: target not used.
    auto bad = make({ctl(0)}); args(bad, 0, 4, 66); issue(bad, "InstructionAlignment");
    auto nonzero = make({exit_inst}); auto parent = launch(nonzero, 0);
    // Repoint only for this test: a nonzero caller preserves itself, as in RTL.
    nonzero.warps[0].status = WarpStatus::Ended;
    auto d = decode(ctl(0)); ok(d); args(nonzero, parent, 2, 64);
    auto effect = evaluate(nonzero, parent, d.value()); ok(effect); commit_effect(nonzero, parent, effect.value());
    require(nonzero.warps[0].pc == 64 && nonzero.warps[0].exec == 1 && nonzero.warps[parent].pc == 4,
            "Nonzero caller excluded from launch slot range");
}
void deferred_spawn() {
    auto machine = make({ctl(0), exit_inst}); auto peer = launch(machine, 4);
    args(machine, 0, 2, 64); const auto old_identity = machine.warps[peer].invocation_id;
    x(machine, peer, 0, 7, 123);
    step(machine);
    require(machine.pending_spawn && machine.warps[0].wait_reason == WaitReason::Spawn && machine.warps[0].pc == 4,
            "Spawn waits, PC already advanced");
    require(!create_warp(machine, {0, 1}), "Host cannot inject during deferred spawn");
    step(machine, peer);
    require(!machine.pending_spawn && machine.warps[0].status == WarpStatus::Ready &&
            machine.warps[peer].pc == 64 && machine.warps[peer].exec == 1 &&
            machine.warps[peer].invocation_id != old_identity && !machine.warps[peer].lanes[0].x_defined[7],
            "Last peer exit triggers fresh spawn without stale registers");
    auto join_only = make({ctl(0), exit_inst}); auto p = launch(join_only, 4);
    args(join_only, 0, 1, 3); step(join_only); step(join_only, p);
    require(!join_only.pending_spawn && join_only.warps[p].status == WarpStatus::Ended, "WSPAWN one waits for older peers without relaunch");
}
void spawn_guards() {
    auto machine = make({ctl(0)}); args(machine, 0, 2, 64); auto peer = launch(machine);
    machine.warps[peer].status = WarpStatus::Faulted; issue(machine, "SpawnSlotInUse");
    auto graphics = make({ctl(0)}); args(graphics, 0, 2, 64); graphics.warps[0].dispatch.stage = ShaderStage::Vertex;
    issue(graphics, "ManagementDuringShader");
    auto bound = make({ctl(0)}); bound.warps[0].code_regions = {{0, 4}};
    args(bound, 0, 2, 64); step(bound);
    issue(bound, "ShaderCodeRange", 1); // Entry is aligned but outside inherited executable code.
}
void barrier_rounds() {
    auto machine = make({ctl(4), ctl(4), exit_inst}); const auto peer = launch(machine);
    args(machine, 0, 0, 2); args(machine, peer, 0, 2);
    step(machine);
    require(machine.warps[0].status == WarpStatus::Waiting && machine.warps[0].pc == 4 &&
            machine.barriers[0].arrived == 1, "First arrival waits at PC+4");
    require(!release_warp(machine, 0), "Cannot release waiting participant");
    step(machine, peer);
    require(machine.warps[0].status == WarpStatus::Ready && machine.warps[peer].status == WarpStatus::Ready &&
            machine.barriers[0].generation == 1 && machine.barriers[0].arrived == 0, "Last arrival releases both");
    step(machine, peer); step(machine);
    require(machine.barriers[0].generation == 2 && machine.warps[0].pc == 8 && machine.warps[peer].pc == 8,
            "Second round with reverse arrival order, no replay");
    auto one = make({ctl(4)}); args(one, 0, 0, 1); step(one);
    require(one.warps[0].status == WarpStatus::Ready && one.barriers[0].generation == 1, "Count one immediately releases");
}
void barrier_ids() {
    auto machine = make({ctl(4), exit_inst}); auto peer = launch(machine);
    args(machine, 0, (2u << 8) | 1u, 1); args(machine, peer, 1, 1);
    step(machine); step(machine, peer);
    require(machine.barriers[10].generation == 1 && machine.barriers[8].generation == 1 &&
            machine.barriers[0].generation == 0, "Packed group/barrier IDs remain independent");
    auto global = make({ctl(4)}); args(global, 0, 0x80000000u, 1); issue(global, "GlobalBarrierUnsupported");
    for (Word invalid : {4u, 8u << 8}) {
        auto capacity = make({ctl(4)}); args(capacity, 0, invalid, 1); issue(capacity, "BarrierCapacity");
    }
}
void barrier_guards() {
    for (Word count : {0u, 5u}) { auto m = make({ctl(4)}); args(m, 0, 0, count); issue(m, "InvalidBarrierCount"); }
    auto mixed = make({ctl(4)}); auto peer = launch(mixed);
    args(mixed, 0, 0, 2); args(mixed, peer, 0, 3); step(mixed); issue(mixed, "BarrierCountMismatch", peer);
    auto partial = make({ctl(4)}); partial.warps[0].exec = 1; args(partial, 0, 0, 1, 0); issue(partial, "BarrierPartialMask");
    auto split = make({ctl(4)}); split.warps[0].sp = 1; args(split, 0, 0, 1); issue(split, "ManagementInsideSplit");
    auto shader = make({ctl(4)}); shader.warps[0].dispatch.stage = ShaderStage::Vertex;
    args(shader, 0, 0, 1); step(shader); require(shader.warps[0].pc == 4, "Reconverged VS/FS BAR remains allowed");
}
void barrier_arrival_orders() {
    // Exhaust all 1!+2!+3!+4! = 33 possible arrival orders, two rounds each.
    for (unsigned count = 1; count <= kWarpSlots; ++count) {
        std::vector<WarpId> order; for (unsigned id = 0; id < count; ++id) order.push_back(id);
        do {
            auto machine = make({ctl(4), ctl(4)});
            for (unsigned id = 1; id < count; ++id) (void)launch(machine);
            for (unsigned id = 0; id < count; ++id) args(machine, id, 0, count);
            for (unsigned round = 0; round < 2; ++round) {
                for (unsigned arrival = 0; arrival < count; ++arrival) {
                    step(machine, order[arrival]);
                    for (unsigned previous = 0; previous <= arrival; ++previous)
                        require(machine.warps[order[previous]].status == (arrival + 1 == count ? WarpStatus::Ready : WarpStatus::Waiting),
                                "No early barrier release for any arrival order");
                }
                require(machine.barriers[0].generation == round + 1, "Round advances exactly once");
            }
        } while (std::next_permutation(order.begin(), order.end()));
    }
}
void deadlock_and_resume() {
    auto machine = make({ctl(4), exit_inst}); args(machine, 0, 0, 2);
    auto report = run_management(machine);
    require(report.reason == StopReason::SuspectedDeadlock && machine.warps[0].pc == 4 && machine.barriers[0].arrived == 1,
            "Missing participant is a preserved deadlock, never fake success");
    const auto peer = launch(machine); args(machine, peer, 0, 2);
    report = run_management(machine);
    require(report.reason == StopReason::Completed && machine.retired_steps == 4 && machine.barriers[0].generation == 1,
            "Host can supply missing peer and resume without replay");
}
void integration() {
    std::vector<Word> code(32, exit_inst);
    const std::vector<Word> root = {
        addi(1, 0, 4), addi(2, 0, 64), ctl(0), addi(3, 0, 1), ctl(1, 3, 0),
        addi(4, 0, 1024), addi(5, 0, 100), 0x00522023u, // SW x5,0(x4), only lane 0.
        addi(3, 0, -1), ctl(1, 3, 0), addi(1, 0, 0), addi(2, 0, 4), ctl(4), exit_inst};
    const std::vector<Word> child = {addi(1, 0, -1), ctl(1, 1, 0), addi(1, 0, 0), addi(2, 0, 4), ctl(4),
                                   addi(4, 0, 1024), 0x00022283u, exit_inst}; // LW x5,0(x4).
    for (unsigned i = 0; i < root.size(); ++i) code[i] = root[i];
    for (unsigned i = 0; i < child.size(); ++i) code[16 + i] = child[i];
    auto machine = make(code); machine.config.max_steps = 12;
    auto report = run_management(machine);
    require(report.reason == StopReason::Timeout && machine.retired_steps == 12, "Pause during concurrent execution");
    machine.config.max_steps = 1000; std::ostringstream trace;
    report = run_management(machine, &trace);
    require(report.reason == StopReason::Completed && machine.retired_steps == 38 && machine.barriers[0].generation == 1,
            "Real machine-code spawn/TMC/barrier completes exactly 38 instructions");
    require(machine.memory.load_le(1024, 4).value() == 100, "Producer store completed");
    for (unsigned id = 1; id < 4; ++id)
        for (const auto& lane : machine.warps[id].lanes) require(lane.x[5] == 100, "Every consumer lane sees pre-barrier store");
    require(trace.str().find("\"wait_reason\":\"barrier\"") != std::string::npos &&
            trace.str().find("\"status\":\"ended\"") != std::string::npos, "Trace exposes waiting and exit");
}
}
int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"decode and reserved fields", decoder}, {"TMC masks, selected lane and exit", tmc_masks},
        {"TMC execution guards", tmc_guards}, {"WSPAWN initialization and pure evaluation", spawn_initialization},
        {"WSPAWN counts and nonzero caller", spawn_counts}, {"deferred WSPAWN and slot reuse", deferred_spawn},
        {"WSPAWN resource guards", spawn_guards}, {"BAR wait/release and repeated rounds", barrier_rounds},
        {"BAR packed identifiers and capacity", barrier_ids}, {"BAR invalid state atomicity", barrier_guards},
        {"BAR all 33 arrival orders over two rounds", barrier_arrival_orders},
        {"deadlock detection and resume", deadlock_and_resume}, {"machine-code producer/consumer and budget resume", integration}};
    unsigned passed = 0;
    for (const auto& [name, run] : cases) {
        try { run(); ++passed; std::cout << "PASS management: " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL management: " << name << ": " << error.what() << '\n'; return 1; }
    }
    std::cout << "PASS management: " << passed << " test groups; instruction coverage 3/3\n";
}
