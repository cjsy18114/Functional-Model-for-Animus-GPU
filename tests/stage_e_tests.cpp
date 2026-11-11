#include "model/submission.hpp"
#include "../tools/submission_fixture.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace animus;
namespace f=animus::fixtures;
namespace {
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
void completed(const RunReport& report) { require(report.reason==StopReason::Completed,report.issue?report.issue->code:"Submission did not complete"); }
void architecture_error(const Machine& machine,const RunReport& report,Word offset,Word category=0) {
    require(report.reason==StopReason::ArchitecturalFault && machine.submission.status==SubmissionStatus::Error,"Expected architectural ERROR");
    require(machine.submission.error_offset==offset,"Exact ERROR_OFFSET");
    if (category) require(machine.submission.error_code==category,"Exact ERROR_CODE v1 category");
}
unsigned count_color(const Machine& machine,Word color) {
    unsigned count=0; for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) count+=f::pixel(machine,x,y)==color; return count;
}
void mmio_lifecycle() {
    auto machine=f::make(); f::commands(machine,{{0,0,0,0},{255,0,0,0}});
    require(mmio_read(machine,12).value()==0,"Reset IDLE");
    f::checked(mmio_write(machine,8,0)); require(machine.submission.status==SubmissionStatus::Idle,"Non-one doorbell ignored");
    f::checked(mmio_write(machine,8,1));
    f::checked(mmio_write(machine,0,0xFFFFFFFF)); f::checked(mmio_write(machine,4,0)); f::checked(mmio_write(machine,8,1));
    require(machine.submission.latched_base==f::kCommands && mmio_read(machine,4).value()==32,"BUSY writes ignored");
    require(!mmio_read(machine,8) && !mmio_write(machine,12,1) && !mmio_read(machine,24),"MMIO access directions");
    completed(run_submission(machine)); require(machine.submission.completed_commands==2 && mmio_read(machine,12).value()==2,"DONE only after END");
    completed(run_submission(machine)); require(machine.submission.completed_commands==2,"No replay after DONE");
    f::checked(mmio_write(machine,0,3)); f::checked(mmio_write(machine,8,1));
    architecture_error(machine,run_submission(machine),0xFFFFFFFF,1);
    f::commands(machine,{{255,0,0,0}}); completed(f::submit(machine)); require(!machine.submission.error_code && !machine.submission.state_valid,"New submission clears errors and state");
}
void command_validation() {
    const std::array<Command,6> invalid={{{0,1,0,0},{0x100,0,0,0},{1,f::kDescriptor,1,0},{2,4,0,0},{3,0,3,1},{4,f::kCompletion,1,1}}};
    for (const auto& command : invalid) { auto machine=f::make(); f::commands(machine,{command,{255,0,0,0}}); architecture_error(machine,f::submit(machine),0,2); }
    auto early=f::make(); f::commands(early,{{255,0,0,0},{0,0,0,0}}); architecture_error(early,f::submit(early),0,2);
    auto missing=f::make(); f::commands(missing,{{0,0,0,0}}); architecture_error(missing,f::submit(missing),16,2);
    auto no_state=f::make(); f::commands(no_state,{{2,0,0,0},{255,0,0,0}}); architecture_error(no_state,f::submit(no_state),0,3);
    auto count=f::make(); f::standard_commands(count,4); architecture_error(count,f::submit(count),32,2);
    auto overflow=f::make(); f::commands(overflow,{{1,f::kDescriptor,0,0},{3,0xFFFFFFFF,3,0},{255,0,0,0}}); architecture_error(overflow,f::submit(overflow),16,4);
}
void state_and_resources() {
    for (unsigned word : {0u,1u,13u,26u}) {
        auto machine=f::make(); auto descriptor=f::descriptor(); descriptor[word]=word==0?2:word==1?8:word==13?9:1;
        f::put_words(machine,f::kDescriptor,descriptor); architecture_error(machine,f::submit(machine),0,3);
        require(!machine.submission.state_valid,"Failed BIND invalidates state");
    }
    auto overlap=f::make(); auto d=f::descriptor(); d[3]=d[2]; f::put_words(overlap,f::kDescriptor,d);
    require(f::submit(overlap).reason==StopReason::ArchitecturalFault,"Attachments cannot alias");
    auto metadata=f::make(); auto resources=metadata.resources; resources.back().role=ResourceRole::Scratch;
    resources.back().base=f::kUniform; resources.back().bytes=16;
    require(!install_resources(metadata,resources),"Scratch cannot overlap inputs");
    f::checked(mmio_write(metadata,8,1)); require(!install_resources(metadata,metadata.resources),"Busy resources remain live");
    auto capacity=f::make(); capacity.config.max_width=4;
    auto report=f::submit(capacity); require(report.reason==StopReason::ModelLimit && capacity.submission.status==SubmissionStatus::Busy,"Model capacity is not hardware ERROR");
    capacity.config.max_width=4096; completed(run_submission(capacity));
    auto byte_vertex=f::make(); f::standard_commands(byte_vertex,3); auto desc=f::descriptor(); desc[11]=1; f::put_words(byte_vertex,f::kDescriptor,desc);
    // Byte-stride vertex addressing is legal; use a shader that reads no FLW.
    f::put_words(byte_vertex,f::kVs,{0xF0000053,0xF00000D3,0xF0000153,0x3F800137,0xF00101D3,0x0000302B});
    completed(f::submit(byte_vertex));
}
void clear_and_fence() {
    auto machine=f::make(); auto state=f::descriptor(); state[22]=3; state[23]=3; state[24]=1; state[25]=1;
    f::put_words(machine,f::kDescriptor,state);
    for (unsigned y=0;y<8;++y) f::put_words(machine,f::kColor+y*40+32,{0xDEADBEEF,0xDEADBEEF});
    f::commands(machine,{{1,f::kDescriptor,0,0},{2,3,0x12345678,0x3F000000},{4,f::kCompletion,7,0},{255,0,0,0}});
    completed(f::submit(machine));
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) require(f::pixel(machine,x,y)==0x12345678 && f::pixel(machine,x,y,true)==0x3F000000,"CLEAR ignores scissor and clears whole target");
    require(machine.memory.load_le(f::kColor+32,4).value()==0xDEADBEEF,"Row padding untouched");
    require(machine.memory.load_le(f::kCompletion,4).value()==7,"FENCE writes after prior work");
    auto ignored=f::make(); f::commands(ignored,{{1,f::kDescriptor,0,0},{2,0,0xFFFFFFFF,0x7FC00000},{255,0,0,0}}); completed(f::submit(ignored));
    auto invalid=f::make(); f::commands(invalid,{{1,f::kDescriptor,0,0},{2,2,0,0x7FC00000},{4,f::kCompletion,9,0},{255,0,0,0}});
    architecture_error(invalid,f::submit(invalid),16,3); require(invalid.memory.load_le(f::kCompletion,4).value()==0,"Failed CLEAR stops later FENCE");
    auto alias=f::make(); f::commands(alias,{{4,f::kUniform,9,0},{255,0,0,0}}); architecture_error(alias,f::submit(alias),0,4);
    auto overlapping=f::make(); auto resources=overlapping.resources;
    resources.push_back({ResourceRole::Completion,f::kUniform,4,3}); f::checked(install_resources(overlapping,resources));
    f::commands(overlapping,{{4,f::kUniform,9,0},{255,0,0,0}}); architecture_error(overlapping,f::submit(overlapping),0,4);
}
void clear_pause_resume() {
    auto machine=f::make(); f::commands(machine,{{1,f::kDescriptor,0,0},{2,1,9,0},{4,f::kCompletion,77,0},{255,0,0,0}});
    machine.config.max_work_items=6; auto report=f::submit(machine);
    require(report.reason==StopReason::Timeout && machine.submission.status==SubmissionStatus::Busy && !machine.submission.error_code,"Budget pause is not ISA ERROR");
    require(machine.submission.clear_index==4 && f::pixel(machine,3,0)==9 && f::pixel(machine,4,0)==0,"Exact CLEAR progress saved");
    // Sentinel probes an already-written pixel; successful resume must not replay it.
    f::put_words(machine,f::kColor,{123}); machine.config.max_work_items=1000;
    completed(run_submission(machine)); require(f::pixel(machine,0,0)==123 && f::pixel(machine,7,7)==9,"Resume starts at next pixel");
    require(machine.memory.load_le(f::kCompletion,4).value()==77,"Completion written after resumed clear");
}
void square_and_shared_edge() {
    auto machine=f::make(); std::ostringstream trace; completed(f::submit(machine,&trace));
    const auto text=trace.str();
    require(text.find("\"invocation_id\":3")!=std::string::npos && text.rfind("\"invocation_id\":2")<text.find("\"invocation_id\":3"),"First primitive FS fully finishes before next primitive VS starts");
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
        const bool inside=x>=2 && x<6 && y>=2 && y<6;
        require(f::pixel(machine,x,y)==(inside?f::kRed:f::kBlack),"Hand-derived 4x4 square footprint");
        require(f::pixel(machine,x,y,true)==(inside?0x3F000000u:0x3F800000u),"D32 depth is 0.5 inside, 1 outside");
    }
    require(machine.submission.completed_primitives==2 && machine.submission.merged_fragments==16,"Shared edge neither double-covered nor cracked");
    require(machine.memory.load_le(f::kCompletion,4).value()==0x12345678 && machine.submission.completed_commands==5,"DRAW completed before FENCE/END");
    for (const auto& warp : machine.warps) require(!warp.allocated,"Shader snapshots consumed and slots released");
}
void depth_and_side_effects() {
    auto machine=f::make();
    // FS writes one scratch counter per pixel before returning red. All counters
    // are beyond the interpolation record, and all active-lane addresses differ.
    f::put_words(machine,f::kFs,{0x0100012B,0x011001AB,0x00319193,0x00218133,0x00211113,
        0x000070B7,0x10008093,0x002080B3,0x00100193,0x0030A023,0xFF000537,0x0FF50513,0x0000302B});
    f::commands(machine,{{1,f::kDescriptor,0,0},{2,3,f::kBlack,0x3E800000},{3,0,6,0},{255,0,0,0}});
    completed(f::submit(machine)); require(count_color(machine,f::kBlack)==64,"0.5 fails LESS against 0.25");
    require(machine.memory.load_le(f::kScratch+256+(2*8+2)*4,4).value()==1,"Late-Z still ran shader side effects");
    require(f::pixel(machine,2,2,true)==0x3E800000,"Failed depth does not write depth");
    auto independent=f::make(); auto state=f::descriptor(); state[1]=2; f::put_words(independent,f::kDescriptor,state);
    completed(f::submit(independent)); require(f::pixel(independent,2,2,true)==0x3F000000,"Depth write independent of depth-test enable");
    auto preserve=f::make(); state=f::descriptor(); state[1]=1; f::put_words(preserve,f::kDescriptor,state);
    completed(f::submit(preserve)); require(f::pixel(preserve,2,2)==f::kRed && f::pixel(preserve,2,2,true)==0x3F800000,"Depth read without depth write");
}
void scissor_viewport_cull() {
    auto machine=f::make(); auto state=f::descriptor(); state[22]=3; state[23]=3; state[24]=2; state[25]=2;
    f::put_words(machine,f::kDescriptor,state); completed(f::submit(machine)); require(count_color(machine,f::kRed)==4,"Scissor half-open 2x2 region");
    auto empty=f::make(); state[24]=0; f::put_words(empty,f::kDescriptor,state); completed(f::submit(empty)); require(count_color(empty,f::kRed)==0,"Empty scissor");
    auto viewport=f::make(); state=f::descriptor(); state[18]=2; state[19]=2; state[20]=4; state[21]=4;
    f::put_words(viewport,f::kDescriptor,state); completed(f::submit(viewport)); require(count_color(viewport,f::kRed)==4,"Viewport maps central square to 2x2");
    auto culled=f::make(); state=f::descriptor(); state[1]=7; f::put_words(culled,f::kDescriptor,state);
    completed(f::submit(culled)); require(count_color(culled,f::kRed)==0,"Visual-clockwise triangles are back-facing");
    auto front=f::make(); f::put_words(front,f::kDescriptor,state);
    for (unsigned triangle=0;triangle<2;++triangle) for (unsigned k=0;k<4;++k) {
        const auto a=f::kVertices+(triangle*3+1)*16+k*4,b=f::kVertices+(triangle*3+2)*16+k*4;
        const auto av=front.memory.load_le(a,4).value(),bv=front.memory.load_le(b,4).value();
        f::checked(front.memory.store_le(a,4,bv)); f::checked(front.memory.store_le(b,4,av));
    }
    completed(f::submit(front)); require(count_color(front,f::kRed)==16,"Visual-CCW is front-facing");
}
void clipping_and_identity() {
    auto machine=f::make(); f::standard_commands(machine,3);
    f::put_float(machine,f::kVertices,-2.0f); // Left-plane crossing generates a clipped polygon.
    f::put_words(machine,f::kFs,{0x0130052B,0x0000302B}); // x10=original primitive_id
    std::ostringstream trace; completed(f::submit(machine,&trace));
    require(machine.submission.completed_primitives==1 && machine.submission.merged_fragments>0,"Clipped primitive processed");
    require(count_color(machine,0)==machine.submission.merged_fragments,"All clipped subtriangles keep original ID zero");
    require(trace.str().find("\"primitive_id\":0")!=std::string::npos,"Original ID traced");
    require(trace.str().find("\"command_offset\":32")!=std::string::npos && trace.str().find("\"raster_profile\":\"reference-v1\"")!=std::string::npos,"Shader trace carries DRAW offset and raster policy");
    auto outside=f::make(); f::standard_commands(outside,3);
    for (unsigned i=0;i<3;++i) f::put_float(outside,f::kVertices+i*16+8,2.0f);
    completed(f::submit(outside)); require(outside.submission.merged_fragments==0,"Far-plane outside triangle rejected");
    auto degenerate=f::make(); f::standard_commands(degenerate,3);
    for (unsigned i=0;i<3;++i) for (unsigned k=0;k<4;++k) f::put_float(degenerate,f::kVertices+i*16+k*4,k==3?1.0f:0.0f);
    completed(f::submit(degenerate)); require(degenerate.submission.merged_fragments==0,"Zero area produces no FS invocation");
    // Independently hand-computable near-plane clipping: one vertex at z=-1/2,
    // two at +1/2 yields two triangles whose new vertices lie exactly on z=0.
    std::array<VertexOutput,3> vertices{};
    const std::array<std::array<float,4>,3> positions={{{-.5f,-.5f,-.5f,1},{.5f,-.5f,.5f,1},{-.5f,.5f,.5f,1}}};
    for (unsigned i=0;i<3;++i) for (unsigned k=0;k<4;++k) vertices[i].position_bits[k]=std::bit_cast<Word>(positions[i][k]);
    auto clipped=clip_triangle(vertices,17,0); f::checked(clipped); require(clipped.value().size()==2,"Near-plane crossing triangulates the clipped quad");
    for (const auto& triangle:clipped.value()) for (const auto& vertex:triangle.vertices) require(vertex.position[2]>=0 && triangle.primitive_id==17,"Clipped vertices satisfy near plane and retain primitive ID");
    auto near=f::make(); f::standard_commands(near,3); f::put_float(near,f::kVertices+8,-.5f); completed(f::submit(near));
    require(near.submission.merged_fragments>0,"Near-plane crossing runs through the complete pipeline");
    auto far=f::make(); f::standard_commands(far,3); f::put_float(far,f::kVertices+8,1.5f); completed(f::submit(far));
    require(far.submission.merged_fragments>0,"Far-plane crossing runs through the complete pipeline");
}
Word fp(unsigned f7,unsigned rd,unsigned a,unsigned b=0) { return (f7<<25)|(b<<20)|(a<<15)|(rd<<7)|0x53; }
void perspective_shader() {
    auto machine=f::make(); auto state=f::descriptor(); state[11]=20; state[13]=1; f::put_words(machine,f::kDescriptor,state); f::standard_commands(machine,3);
    f::put_words(machine,f::kVs,{0x001000AB,0x0000A007,0x0040A087,0x0080A107,0x00C0A187,0x0100A207,0x0000302B});
    const std::array<float,3> w={1.0f,2.0f,std::bit_cast<float>(Word{0x3F2AAAAB})};
    const std::array<std::array<float,2>,3> xy={{{-0.5f,-0.5f},{0.5f,-0.5f},{-0.5f,0.5f}}};
    for (unsigned i=0;i<3;++i) {
        f::put_float(machine,f::kVertices+i*20,xy[i][0]*w[i]); f::put_float(machine,f::kVertices+i*20+4,xy[i][1]*w[i]);
        f::put_float(machine,f::kVertices+i*20+8,w[i]*0.5f); f::put_float(machine,f::kVertices+i*20+12,w[i]);
        f::put_float(machine,f::kVertices+i*20+16,i==2?1.0f:0.0f);
    }
    std::vector<Word> shader={0x014000AB,0x0000A087,0x0040A107,0x0080A187,
        0x0150012B,fp(120,4,2),0x0160012B,fp(120,5,2),0x3F800137,fp(120,6,2),
        fp(4,6,6,4),fp(4,6,6,5),fp(8,7,1,4),fp(8,8,2,5),fp(0,7,7,8),fp(8,8,3,6),fp(0,7,7,8),
        (32u<<25)|(7u<<15)|(7u<<7)|0x0B,0x00C0A087,0x0100A107,0x0140A187,
        fp(8,8,1,4),fp(8,9,2,5),fp(0,8,8,9),fp(8,9,3,6),fp(0,8,8,9),fp(8,8,8,7),fp(112,10,8),0x0000302B};
    f::put_words(machine,f::kFs,shader); completed(f::submit(machine));
    // At (2.5,2.5), lambda=(3/4,1/8,1/8), f32 inv_w=(1,1/2,3/2).
    // q=1, numerator=3/16, so exact perspective result is 0.1875 (not affine 0.125).
    require(f::pixel(machine,2,2)==0x3E400000,"Actual FS performs perspective interpolation from unchanged FP32 record layout");
    require(machine.memory.load_le(f::kScratch,4).value()==0x3F800000 && machine.memory.load_le(f::kScratch+8,4).value()==0x3FC00000,"Unscaled inv_w record");
}
void texture_draw() {
    auto machine=f::make(); auto state=f::descriptor(); state[14]=f::kTexture; state[15]=3; state[16]=2; state[17]=16;
    f::put_words(machine,f::kDescriptor,state); f::put_words(machine,f::kTexture,{0xFF0000FF,0xFF00FF00,0xFFFF0000,0xDEADBEEF,0xFFFFFFFF,0xFF000000,0xFFFF00FF});
    f::put_words(machine,f::kUniform,{0x3F000000,0});
    f::put_words(machine,f::kFs,{0x002000AB,0x0000A103,0x0040A183,0x0031252B,0x0000302B});
    completed(f::submit(machine)); require(count_color(machine,0xFF00FF00)==16,"E pipeline binds texture and runs actual gfx.tex shader");
}
void shader_fault_and_no_fence() {
    auto machine=f::make(); f::put_words(machine,f::kFs,{0x0000102B});
    architecture_error(machine,f::submit(machine),32,5);
    require(count_color(machine,f::kBlack)==64 && machine.memory.load_le(f::kCompletion,4).value()==0,"Earlier CLEAR retained, later FENCE suppressed");
    require(machine.submission.last_issue->pc==f::kFs && machine.submission.last_issue->command_offset==32,"Shader PC plus command offset diagnostic");
    auto readonly=f::make(); f::put_words(readonly,f::kFs,{0x002000AB,0x0000A023,0x0000302B});
    require(f::submit(readonly).reason==StopReason::ArchitecturalFault,"Guest cannot write read-only registered input despite broad fixture RAM mapping");
    auto interpolation=f::make(); f::put_words(interpolation,f::kFs,{0x014000AB,0x0000A023,0x0000302B});
    architecture_error(interpolation,f::submit(interpolation),32,5);
    require(interpolation.submission.last_issue->code=="ReadOnlyInterpolation","Internal RW scratch does not make the live interpolation record shader-writable");
}
void draw_pause_resume() {
    auto baseline=f::make(); completed(f::submit(baseline));
    auto paused=f::make(); paused.config.max_steps=5;
    require(f::submit(paused).reason==StopReason::Timeout && paused.submission.status==SubmissionStatus::Busy,"Shader budget pauses mid-VS");
    const auto attempts=paused.submission.shader_attempts;
    paused.config.max_steps=1000; paused.config.max_work_items=90;
    auto report=run_submission(paused); require(report.reason==StopReason::Timeout,"Work budget pauses in DRAW");
    require(paused.submission.shader_attempts>=attempts && !paused.submission.error_code,"Progress and no false ERROR");
    paused.config.max_work_items=10000; completed(run_submission(paused));
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) require(f::pixel(paused,x,y)==f::pixel(baseline,x,y) && f::pixel(paused,x,y,true)==f::pixel(baseline,x,y,true),"Resumed outputs match uninterrupted draw");
    require(paused.submission.merged_fragments==16 && paused.submission.shader_attempts==baseline.submission.shader_attempts,"No duplicated fragments or shader instructions");
}
void profiles_limits_and_empty_draw() {
    auto disabled=f::make(); disabled.config.raster_profile=RasterProfile::Disabled;
    require(f::submit(disabled).reason==StopReason::ModelLimit && disabled.submission.offset==32,"No implicit raster profile");
    disabled.config.raster_profile=RasterProfile::ReferenceV1; completed(run_submission(disabled));
    auto empty=f::make(); empty.config.raster_profile=RasterProfile::Disabled; f::standard_commands(empty,0); completed(f::submit(empty));
    auto nan=f::make(); f::checked(nan.memory.store_le(f::kVertices,4,0x7FC00000));
    require(f::submit(nan).reason==StopReason::ModelLimit && !nan.submission.error_code && nan.submission.status==SubmissionStatus::Busy,"Nonfinite geometry not silently changed or misclassified");
    auto singular=f::make(); f::standard_commands(singular,3);
    for (unsigned i=0;i<3;++i) f::put_words(singular,f::kVertices+i*16,{0,0,0,0});
    auto unsupported=f::submit(singular); require(unsupported.reason==StopReason::ModelLimit && unsupported.issue->code=="ProjectionSingularity","w=0 remains an explicit geometric model limit");
    auto scratch=f::make(); for (auto& resource:scratch.resources) if (resource.role==ResourceRole::Scratch) resource.bytes=4;
    require(f::submit(scratch).reason==StopReason::ModelLimit,"Scratch capacity does not overwrite guest memory");
    auto narrow=f::make(); narrow.config.warp_width=2; completed(f::submit(narrow)); require(count_color(narrow,f::kRed)==16,"Vertex and FS batches honor small test width");
}
void host_rounding_and_resource_guards() {
    const int prior=std::fegetround(); std::fesetround(FE_DOWNWARD);
    auto machine=f::make(); completed(f::submit(machine)); require(std::fegetround()==FE_DOWNWARD,"Geometry restores host rounding"); std::fesetround(prior);
    require(count_color(machine,f::kRed)==16,"Coverage independent of host rounding");
    auto invalid=f::make(); auto resources=invalid.resources; resources[0].bytes=~std::uint64_t{0}; require(!install_resources(invalid,resources),"Host metadata rejects arithmetic overflow");
}
}
int main() {
    struct Test { const char* name; void(*run)(); };
    const Test tests[]={{"E1 MMIO lifecycle",mmio_lifecycle},{"E1 command validation",command_validation},{"E1 state and resources",state_and_resources},
        {"E2 clear and fence",clear_and_fence},{"E2 clear pause/resume",clear_pause_resume},{"E3 square and shared edge",square_and_shared_edge},
        {"E3 Late-Z and side effects",depth_and_side_effects},{"E4 scissor, viewport and cull",scissor_viewport_cull},
        {"E4 clipping and identity",clipping_and_identity},{"E3 perspective shader",perspective_shader},{"E3 texture draw",texture_draw},
        {"E shader fault and no fence",shader_fault_and_no_fence},{"E draw pause/resume",draw_pause_resume},
        {"E profiles, limits and empty draw",profiles_limits_and_empty_draw},{"E host environment and metadata guards",host_rounding_and_resource_guards}};
    unsigned failed=0;
    for (const auto& test:tests) {
        try { test.run(); std::cout<<"PASS "<<test.name<<'\n'; }
        catch (const std::exception& error) { ++failed; std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n'; }
    }
    return failed?1:0;
}
