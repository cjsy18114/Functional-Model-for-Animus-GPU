#include "model/model.hpp"
#include "model/execution.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace animus;
namespace {
constexpr Word kEnd = 0x0000302B;
constexpr Word kTexture = 0x0020A52B; // gfx.tex x10,x1,x2, reserved high fields zero.
const std::array<Word, 6> kColors = {0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF,0xFF000000,0xFFFF00FF};
std::set<Op> executed;
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class T> void ok(const Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}
Word get(unsigned selector, unsigned destination = 3) { return (selector << 20) | (destination << 7) | 0x2B; }
Result<StepRecord> step(Machine& machine, WarpId id = 0) {
    auto result = step_shader(machine, id);
    if (result) executed.insert(result.value().op);
    return result;
}
Machine base(std::initializer_list<Word> program) {
    ModelConfig config; config.memory_bytes = 8192;
    auto result = create_machine(config); ok(result); auto machine = std::move(result.value());
    std::vector<std::uint8_t> code(512, 0);
    unsigned offset = 0;
    for (const auto word : program) for (unsigned byte = 0; byte < 4; ++byte)
        code.at(offset++) = static_cast<std::uint8_t>(word >> (8 * byte));
    ok(machine.memory.load_image(0, code, permission(Access::Read) | permission(Access::Execute)));
    ok(machine.memory.map_region({1024,4096,permission(Access::Read) | permission(Access::Write)}));
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 3; ++x)
        ok(machine.memory.store_le(2048 + y * 16 + x * 4,4,kColors[y * 3 + x]));
    return machine;
}
LaunchInfo launch_info(ShaderStage stage, Mask mask = 0xFFFFFFFFu, unsigned varying = 0) {
    LaunchInfo launch; launch.init_mask = mask; launch.dispatch.stage = stage;
    launch.dispatch.varying_count = varying;
    launch.dispatch.graphics = {2048,3,2,16};
    for (unsigned lane = 0; lane < 32; ++lane) {
        auto& input = launch.dispatch.inputs[lane];
        input.vertex_id = 100 + lane; input.vertex_addr = 1024 + lane * 4;
        input.uniform_base = 1536; input.pixel_x = lane; input.pixel_y = 77;
        input.depth_bits = 0x3F000000 + lane; input.primitive_id = 9;
        input.prim_data_addr = 3072; input.bary0_bits = 0x7FC00010 + lane; input.bary1_bits = 0x3E800000;
    }
    return launch;
}
WarpId launch(Machine& machine, ShaderStage stage, Mask mask = 0xFFFFFFFFu, unsigned varying = 0) {
    auto result = create_warp(machine,launch_info(stage,mask,varying)); ok(result);
    require(result.value().has_value(),"Expected a nonempty dispatch"); return *result.value();
}
void initialize_outputs(Machine& machine, WarpId id, unsigned count) {
    for (unsigned lane = 0; lane < 32; ++lane) {
        if (!(machine.warps[id].init_mask & (Mask{1} << lane))) continue;
        for (unsigned reg = 0; reg < count; ++reg) ok(initialize_f(machine,id,lane,reg,0x3F000000 + reg * 32 + lane));
        ok(initialize_x(machine,id,lane,10,0xFF000000 + lane));
    }
}
void expect_issue(Machine& machine, const std::string& code, WarpId id = 0) {
    const auto before = machine.warps[id]; const auto retired = machine.retired_steps;
    auto result = step(machine,id);
    require(!result && result.error().code == code,"Expected " + code);
    const auto& after = machine.warps[id];
    require(after.pc == before.pc && !after.completed_outputs && machine.retired_steps == retired,"No partial output/control retirement on failure");
    for (unsigned lane = 0; lane < 32; ++lane)
        require(after.lanes[lane].x == before.lanes[lane].x && after.lanes[lane].f_bits == before.lanes[lane].f_bits,"No partial register commit");
}
void decoder_contracts() {
    for (const auto& item : {std::pair{0x002001ABu,Op::GraphicsGet}, {kTexture,Op::GraphicsTexture},{kEnd,Op::GraphicsEnd}}) {
        auto decoded = decode(item.first); ok(decoded); require(decoded.value().op == item.second,"Fixed D machine word");
    }
    auto high = decode(get(0xFFF)); ok(high); require(high.value().selector == 4095,"Selector is unsigned twelve bits");
    for (const Word word : {get(2)|0x8000u,kTexture|0x02000000u,kTexture|0x08000000u,kEnd|0x80u,kEnd|0x100000u,0x0000102Bu,0x0000402Bu}) {
        auto result = decode(word); require(!result && result.error().kind == IssueKind::ArchitecturalFault,"Reserved graphics fields fault");
    }
}
void selector_table() {
    const auto vs = launch_info(ShaderStage::Vertex).dispatch;
    const auto fs = launch_info(ShaderStage::Fragment).dispatch;
    const auto& v = vs.inputs[31]; const auto& f = fs.inputs[31];
    const std::array<Word,3> vertex = {v.vertex_id,v.vertex_addr,v.uniform_base};
    for (unsigned selector = 0; selector < 3; ++selector) {
        auto value = get_input(vs,31,static_cast<std::uint16_t>(selector)); ok(value); require(value.value() == vertex[selector],"VS selectors");
    }
    const std::array<Word,7> fragment = {f.pixel_x,f.pixel_y,f.depth_bits,f.primitive_id,f.prim_data_addr,f.bary0_bits,f.bary1_bits};
    for (unsigned i = 0; i < 7; ++i) { auto value = get_input(fs,31,static_cast<std::uint16_t>(16+i)); ok(value); require(value.value() == fragment[i],"FS selectors preserve bits"); }
    ok(get_input(fs,0,2));
    require(!get_input(vs,0,16) && !get_input(fs,0,0) && !get_input(fs,0,0x17) && !get_input(fs,32,2),"Wrong stage/undefined selector/lane rejected");
}
void dispatch_validation() {
    auto machine = base({kEnd}); auto info = launch_info(ShaderStage::Vertex,1);
    info.dispatch.varying_count = 9; require(!create_warp(machine,info),"Varying bound");
    info = launch_info(ShaderStage::Vertex,1); info.dispatch.inputs[0].vertex_addr = 8192;
    require(!create_warp(machine,info),"Vertex minimum extent checked");
    info = launch_info(ShaderStage::Fragment,1,8); info.dispatch.inputs[0].prim_data_addr = 5104;
    require(!create_warp(machine,info),"Full interpolation record extent checked");
    info = launch_info(ShaderStage::Vertex,1); info.dispatch.inputs[31].vertex_addr = 0xFFFFFFFF;
    auto result = create_warp(machine,info); ok(result); require(*result.value() == 0,"Inactive input ignored and failed launches consume no slots");
    info.dispatch.graphics.texture_width = 0; require(!create_warp(machine,info),"Texture dimensions paired");
    info = launch_info(ShaderStage::Vertex,1); info.dispatch.graphics.texture_width = 4097;
    require(!create_warp(machine,info),"Host texture dimension limit");
}
void input_execution() {
    auto machine = base({get(0),get(1),get(2)}); launch(machine,ShaderStage::Vertex);
    ok(step(machine)); for (unsigned lane = 0; lane < 32; ++lane) require(machine.warps[0].lanes[lane].x[3] == 100+lane,"All-lane distinct input");
    ok(step(machine)); ok(step(machine));
    auto fs = base({get(0x15)}); launch(fs,ShaderStage::Fragment,0x80000000);
    ok(step(fs)); require(fs.warps[0].lanes[31].x[3] == 0x7FC0002F && !fs.warps[0].lanes[0].x_defined[3],"Raw FP bits, sparse mask");
    auto zero = base({get(0,0)}); launch(zero,ShaderStage::Vertex,1); ok(step(zero)); require(zero.warps[0].lanes[0].x[0] == 0,"x0 discarded");
}
void input_faults() {
    auto machine = base({get(0xFFF,0)}); launch(machine,ShaderStage::Vertex); expect_issue(machine,"InvalidSelector");
    auto wrong = base({get(0)}); launch(wrong,ShaderStage::Fragment); expect_issue(wrong,"WrongStage");
    auto test = base({get(2)}); auto created = create_warp(test,{}); ok(created); expect_issue(test,"WrongStage");
}
void vertex_exports_and_reuse() {
    for (unsigned count : {0u,8u}) {
        auto machine = base({kEnd}); launch(machine,ShaderStage::Vertex,0xFFFFFFFFu,count);
        initialize_outputs(machine,0,4+count); auto record = step(machine); ok(record);
        const auto& batch = *record.value().outputs;
        require(batch.vertices.size() == 32 && batch.fragments.empty(),"Exactly 32 VS exports");
        for (unsigned lane = 0; lane < 32; ++lane) {
            const auto& vertex = batch.vertices[lane];
            require(vertex.lane == lane && vertex.vertex_id == 100+lane && vertex.position_bits[0] == 0x3F000000+lane && vertex.varying_count == count,"VS order/identity/position");
            if (count) require(vertex.varying_bits[7] == 0x3F000000+11*32+lane,"Last varying captured");
        }
        auto saved = release_warp(machine,0); ok(saved);
        launch(machine,ShaderStage::Fragment,1); initialize_outputs(machine,0,0);
        require(!machine.warps[0].completed_outputs && saved.value().completed_outputs->vertices[31].vertex_id == 131,"Owned output survives cleared/reused slot");
        require(batch.invocation_id != machine.warps[0].invocation_id,"Old/new output identity distinct");
    }
}
void fragment_exports() {
    auto machine = base({kEnd,0x00100513}); launch(machine,ShaderStage::Fragment,0x80000001);
    initialize_outputs(machine,0,0); auto result = step(machine); ok(result);
    const auto& outputs = result.value().outputs->fragments;
    require(outputs.size() == 2 && outputs[0].lane == 0 && outputs[1].lane == 31,"Only INIT_MASK exports, ordered");
    require(outputs[1].input.depth_bits == 0x3F00001F && outputs[1].input.pixel_y == 77 && outputs[1].rgba8 == 0xFF00001F,"Original fragment context preserved");
    require(machine.warps[0].status == WarpStatus::Ended && machine.warps[0].pc == 4,"End retires once");
    auto again = step_shader(machine,0); require(!again && machine.retired_steps == 1,"No instruction after end");
}
void export_faults() {
    auto stack = base({kEnd}); launch(stack,ShaderStage::Vertex); initialize_outputs(stack,0,4);
    stack.warps[0].sp = 1; expect_issue(stack,"UnreconvergedEnd");
    auto mask = base({kEnd}); launch(mask,ShaderStage::Fragment); initialize_outputs(mask,0,0);
    mask.warps[0].exec = 1; expect_issue(mask,"UnreconvergedEnd");
    auto undefined = base({kEnd}); launch(undefined,ShaderStage::Vertex); initialize_outputs(undefined,0,4);
    undefined.warps[0].lanes[31].f_defined[3] = false; expect_issue(undefined,"UndefinedGraphicsOutput");
    require(undefined.warps[0].status == WarpStatus::Ready,"Undefined-output check is a host diagnostic");
    undefined.config.check_undefined = false; ok(step(undefined));
    auto missing_color = base({kEnd}); launch(missing_color,ShaderStage::Fragment,1); expect_issue(missing_color,"UndefinedGraphicsOutput");
}
void texture_boundaries() {
    auto machine = base({kEnd}); const GraphicsState texture{2048,3,2,16};
    struct Case { Word u,v; unsigned index; };
    const Case cases[] = {{0,0,0},{0x3F800000,0,2},{0,0x3F800000,3},{0x3F800000,0x3F800000,5},
        {0xBF800000,0,0},{0xFF800000,0,0},{0x7F800000,0,2},{0xFFC01234,0,0},{0x7F800001,0,0},
        {1,0,0},{0x80000000,0,0},{0x3EAAAAAA,0,0},{0x3EAAAAAB,0,1},{0x3F7FFFFF,0,2},{0,0x3F000000,3}};
    for (const auto& item : cases) { auto value = sample_texture(machine.memory,texture,item.u,item.v); ok(value); require(value.value() == kColors[item.index],"Exact UV boundary and padded stride"); }
    auto one = sample_texture(machine.memory,{2048,1,1,4},0x7F800000,0x7FC00000); ok(one); require(one.value() == kColors[0],"1x1 texture");
}
void texture_failures() {
    auto machine = base({kTexture});
    for (const GraphicsState texture : {GraphicsState{2048,0,0,0},{2048,3,2,8},{2049,3,2,16},{2048,3,2,13},{0xFFFFFFFC,2,1,8},{5100,3,2,16}})
        require(!sample_texture(machine.memory,texture,0,0),"Invalid texture extent/layout rejected");
    ok(machine.memory.map_region({6144,32,permission(Access::Write)}));
    require(!sample_texture(machine.memory,{6144,3,2,16},0,0),"Texture read permission checked");
    auto info = launch_info(ShaderStage::Fragment,1); info.dispatch.graphics = {};
    auto id = create_warp(machine,info); ok(id); ok(initialize_x(machine,0,0,1,0)); ok(initialize_x(machine,0,0,2,0));
    expect_issue(machine,"EmptyTexture");
}
void texture_instruction() {
    auto machine = base({kTexture,kEnd}); launch(machine,ShaderStage::Fragment);
    for (unsigned lane = 0; lane < 32; ++lane) {
        ok(initialize_x(machine,0,lane,1,lane%2 ? 0x3F800000 : 0));
        ok(initialize_x(machine,0,lane,2,lane%4 >= 2 ? 0x3F800000 : 0));
    }
    ok(step(machine)); auto ended = step(machine); ok(ended);
    for (const auto& output : ended.value().outputs->fragments)
        require(output.rgba8 == kColors[(output.lane%4>=2 ? 3 : 0)+(output.lane%2 ? 2 : 0)],"Texture directly feeds FS x10 original packed color");
    auto alias = base({0x0025252B}); launch(alias,ShaderStage::Vertex,0x80000000); // gfx.tex x10,x10,x2
    ok(initialize_x(alias,0,31,10,0x3F800000)); ok(initialize_x(alias,0,31,2,0)); ok(step(alias));
    require(alias.warps[0].lanes[31].x[10] == kColors[2] && !alias.warps[0].lanes[0].x_defined[10],"UV destination alias, VS sampling, inactive lanes");
}
void staged_commit_and_trace() {
    auto machine = base({kEnd}); launch(machine,ShaderStage::Vertex,1,1); initialize_outputs(machine,0,5);
    auto decoded = decode(kEnd); ok(decoded); auto effect = evaluate(machine,0,decoded.value()); ok(effect);
    require(!machine.warps[0].completed_outputs && machine.warps[0].status == WarpStatus::Ready,"evaluate does not publish output");
    auto record = step(machine); ok(record); std::ostringstream stream; write_trace(stream,record.value());
    require(stream.str().find("\"ended\":true") != std::string::npos && stream.str().find("\"stage\":\"vs\"") != std::string::npos && stream.str().find("\"varying_bits\":[") != std::string::npos,"Output and completion traced");
    // Run a complete VS from machine code: gfx.get VERTEX_ADDR, five FLWs,
    // gfx.end. The host provides vertex bytes, never computes shader outputs.
    auto vs = base({0x001000AB,0x0000A007,0x0040A087,0x0080A107,0x00C0A187,0x0100A207,kEnd});
    auto info = launch_info(ShaderStage::Vertex,0xFFFFFFFFu,1);
    for (unsigned lane = 0; lane < 32; ++lane) {
        info.dispatch.inputs[lane].vertex_addr = 1024+lane*20;
        for (unsigned field = 0; field < 5; ++field) ok(vs.memory.store_le(1024+lane*20+field*4,4,0x3F800000+lane*5+field));
    }
    auto launched = create_warp(vs,info); ok(launched);
    info.dispatch.inputs[0].vertex_addr = 0xFFFFFFFC; // Launch must have copied, not referenced the host context.
    const std::array<WarpId,1> ids = {0};
    auto report = run_shader_batch(vs,ids);
    require(report.reason == StopReason::Completed && report.retired_steps == 7,"Machine-code VS completes");
    for (unsigned lane = 0; lane < 32; ++lane) {
        const auto& output = vs.warps[0].completed_outputs->vertices[lane];
        require(output.position_bits[3] == 0x3F800000+lane*5+3 && output.varying_bits[0] == 0x3F800000+lane*5+4,"Machine-code VS preserves all position/varying bytes");
    }
}
void batch_running() {
    auto machine = base({get(2),kEnd});
    for (unsigned id = 0; id < 4; ++id) { launch(machine,id%2 ? ShaderStage::Fragment : ShaderStage::Vertex,Mask{1}<<id); initialize_outputs(machine,id,4); }
    const std::array<WarpId,4> ids = {0,1,2,3};
    auto report = run_shader_batch(machine,ids); require(report.reason == StopReason::Completed && report.retired_steps == 8,"Four shader warps finish via gfx.end");
    report = run_shader_batch(machine,ids); require(report.reason == StopReason::Completed && report.retired_steps == 0,"Completed batch does not export twice");
    for (unsigned id = 0; id < 4; ++id) { require(machine.warps[id].completed_outputs->invocation_id == machine.warps[id].invocation_id,"Output ownership"); ok(release_warp(machine,id)); }
    auto timeout = base({0x0000006F}); launch(timeout,ShaderStage::Fragment,1); timeout.config.max_steps = 3;
    const std::array<WarpId,1> one = {0}; require(run_shader_batch(timeout,one).reason == StopReason::Timeout,"Shared instruction attempt budget");
    timeout.warps[0].status = WarpStatus::Waiting; require(run_shader_batch(timeout,one).reason == StopReason::SuspectedDeadlock,"No-ready diagnosis");
    auto stopped = base({kEnd}); launch(stopped,ShaderStage::Fragment,1); initialize_outputs(stopped,0,0);
    require(run_until_stop(stopped,0,100).reason == StopReason::Completed,"Single-warp runner recognizes shader completion before host stop");
}
void instruction_coverage() {
    for (const auto op : {Op::GraphicsGet,Op::GraphicsTexture,Op::GraphicsEnd}) require(executed.contains(op),"Missing D instruction execution");
    std::cout << "D instruction coverage: 3/3\n";
}
}
int main() {
    struct Test { const char* name; void(*run)(); };
    const Test tests[] = {{"D decoder contracts",decoder_contracts},{"D1 selector table",selector_table},
        {"D1 dispatch validation",dispatch_validation},{"D1 input execution",input_execution},{"D1 input faults",input_faults},
        {"D2 vertex exports and reuse",vertex_exports_and_reuse},{"D2 fragment exports",fragment_exports},
        {"D2 export faults and diagnostics",export_faults},{"D3 texture boundaries",texture_boundaries},
        {"D3 texture failures",texture_failures},{"D3 texture instruction",texture_instruction},
        {"D output commit and trace",staged_commit_and_trace},{"D shader batch running",batch_running},
        {"D instruction coverage",instruction_coverage}};
    unsigned failed = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
