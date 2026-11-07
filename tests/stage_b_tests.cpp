#include "model/model.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace animus;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class T> void ok(const Result<T>& value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
}
// Encoders construct test programs; separate fixed raw vectors below validate the decoder independently.
Word reg(unsigned f3, unsigned f7=0, unsigned rd=3, unsigned a=1, unsigned b=2) {
    return (f7<<25)|(b<<20)|(a<<15)|(f3<<12)|(rd<<7)|0x33;
}
Word imm(unsigned f3, Word immediate, unsigned rd=3, unsigned a=1) {
    return ((immediate&0xFFFu)<<20)|(a<<15)|(f3<<12)|(rd<<7)|0x13;
}
Word branch(unsigned f3, int offset, unsigned a=1, unsigned b=2) {
    const Word u=static_cast<Word>(offset);
    return (((u>>12)&1)<<31)|(((u>>5)&63)<<25)|(b<<20)|(a<<15)|(f3<<12)|(((u>>1)&15)<<8)|(((u>>11)&1)<<7)|0x63;
}
Word jal(int offset, unsigned rd=0) {
    const Word u=static_cast<Word>(offset);
    return (((u>>20)&1)<<31)|(((u>>1)&1023)<<21)|(((u>>11)&1)<<20)|(u&0xFF000)|(rd<<7)|0x6F;
}
Word jalr(unsigned rd, unsigned a, int offset=0) { return ((Word(offset)&0xFFF)<<20)|(a<<15)|(rd<<7)|0x67; }
Word load(unsigned f3, int offset=0, unsigned rd=3) { return ((Word(offset)&0xFFF)<<20)|(1<<15)|(f3<<12)|(rd<<7)|3; }
Word store(unsigned f3, int offset=0) {
    const Word u=Word(offset)&0xFFF;
    return ((u>>5)<<25)|(2<<20)|(1<<15)|(f3<<12)|((u&31)<<7)|0x23;
}
Word split(unsigned predicate=1, unsigned target=2) { return (2u<<25)|(target<<20)|(predicate<<15)|0x0B; }
constexpr Word join=0x0600000Bu;
Word csr(unsigned operation=2, unsigned source=0, unsigned rd=3) { return 0x80000073u|(operation<<12)|(source<<15)|(rd<<7); }
Machine make(std::initializer_list<Word> program, Mask mask=0xFFFFFFFFu) {
    ModelConfig config; config.memory_bytes=4096;
    auto created=create_machine(config); ok(created);
    auto machine=std::move(created.value());
    std::vector<std::uint8_t> code(256,0);
    unsigned address=0;
    for(Word word:program) { for(unsigned i=0;i<4;++i) code.at(address++)=static_cast<std::uint8_t>(word>>(8*i)); }
    ok(machine.memory.load_image(0,code,permission(Access::Read)|permission(Access::Execute)));
    ok(machine.memory.map_region({512,2048,permission(Access::Read)|permission(Access::Write)}));
    auto launched=create_warp(machine,{0,mask}); ok(launched);
    return machine;
}
void fill(Machine& machine, unsigned r, Word value) {
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(machine,0,lane,r,value));
}
void expect_fault(Machine& machine, const std::string& code) {
    const auto pc=machine.warps[0].pc; const auto mask=machine.warps[0].exec; const auto sp=machine.warps[0].sp;
    const auto retired=machine.retired_steps;
    auto result=step_shader(machine,0);
    require(!result && result.error().code==code,"Expected "+code);
    require(machine.warps[0].pc==pc && machine.warps[0].exec==mask && machine.warps[0].sp==sp,"Fault must preserve PC/mask/SP");
    require(machine.retired_steps==retired,"Fault must not retire");
}
void decode_contracts() {
    struct Vector { Word raw; Op op; Word immediate; };
    const Vector vectors[]={
        {0x002081B3,Op::Add,0},{0x402081B3,Op::Subtract,0},{0xFFF0A193,Op::LessSignedImmediate,0xFFFFFFFF},
        {0x8000C193,Op::XorImmediate,0xFFFFF800},{0x41F0D193,Op::ShiftRightArithmeticImmediate,31},
        {0xFFFFF1B7,Op::Lui,0xFFFFF000},{0x00001197,Op::Auipc,0x1000},
        {0xFE20AE23,Op::StoreWord,0xFFFFFFFC},{0xFE20CEE3,Op::BranchLessSigned,0xFFFFFFFC},
        {0xFFDFF0EF,Op::JumpLink,0xFFFFFFFC},{0x0040A183,Op::LoadWord,4},
        {0x022081B3,Op::Multiply,0},{0x800021F3,Op::ReadThreadMask,0},
        {0x0420800B,Op::Split,0},{0x0600000B,Op::Join,0}};
    for(const auto& v:vectors) { auto d=decode(v.raw); ok(d); require(d.value().op==v.op && d.value().immediate_bits==v.immediate,"Fixed decode vector"); }
    const Word invalid[]={0x02009193,0x40009193,0x0020B1B3|(2u<<25),0x0000B183,0x0020B023,0x0020A063,0x000091E7,
                          0x0420808B,0x0420900B,0x0600800B,0x0610000B,0x800041F3};
    for(Word raw:invalid) { auto d=decode(raw); require(!d && d.error().kind==IssueKind::ArchitecturalFault,"Reserved B encoding rejected"); }
    require(decode(branch(0,-4096)).value().immediate_bits==0xFFFFF000,"B minimum immediate");
    require(decode(branch(0,4094)).value().immediate_bits==4094,"B maximum immediate");
    require(decode(jal(-1048576)).value().immediate_bits==0xFFF00000,"J minimum immediate");
    require(decode(jal(1048574)).value().immediate_bits==1048574,"J maximum immediate");
    require(decode(0x00002007).value().op==Op::LoadFloat,"FLW is distinct from integer loads");
}
void alu_vectors() {
    struct Vector { Word instruction,a,b,result; };
    const Vector vectors[]={
        {reg(0),0xFFFFFFFF,2,1},{reg(0,32),0,1,0xFFFFFFFF},{reg(1),1,63,0x80000000},
        {reg(2),0xFFFFFFFF,0,1},{reg(3),0xFFFFFFFF,0,0},{reg(4),0xFF00,0xFFFF,0xFF},
        {reg(5),0x80000000,31,1},{reg(5,32),0x80000000,31,0xFFFFFFFF},{reg(6),0x100,1,0x101},
        {reg(7),0xFF,0x12,0x12},{imm(0,0xFFF),0,0,0xFFFFFFFF},{imm(2,0xFFF),0xFFFFFFFE,0,1},
        {imm(3,0xFFF),1,0,1},{imm(4,0xFFF),0xFF,0,0xFFFFFF00},{imm(6,0x800),0xFF,0,0xFFFFF8FF},
        {imm(7,0xFFF),0xAB,0,0xAB},{imm(1,31),1,0,0x80000000},{imm(5,31),0x80000000,0,1},
        {imm(5,0x400),0x80000000,0,0x80000000},{imm(5,0x41F),0x80000000,0,0xFFFFFFFF},
        {0x800001B7,0,0,0x80000000},{0x00001197,0,0,0x1000}};
    for(const auto& v:vectors) {
        auto machine=make({v.instruction}); fill(machine,1,v.a); fill(machine,2,v.b); ok(step_shader(machine,0));
        require(machine.warps[0].lanes[31].x[3]==v.result,"ALU "+std::string(op_name(decode(v.instruction).value().op)));
    }
    auto lane_values=make({reg(0)});
    for(unsigned lane=0;lane<32;++lane) { ok(initialize_x(lane_values,0,lane,1,lane)); ok(initialize_x(lane_values,0,lane,2,100+lane)); }
    ok(step_shader(lane_values,0));
    for(unsigned lane=0;lane<32;++lane) require(lane_values.warps[0].lanes[lane].x[3]==100+2*lane,"Lane-local ALU inputs");
    auto pc_relative=make({imm(0,0,0,0),0x00001197});
    ok(step_shader(pc_relative,0)); ok(step_shader(pc_relative,0));
    require(pc_relative.warps[0].lanes[0].x[3]==4100,"AUIPC uses instruction PC");
}
void load_store_widths() {
    const unsigned load_functions[]={0,1,2,4,5};
    const Word expected[]={0xFFFFFF80,0xFFFFFF80,0x1234FF80,0x80,0xFF80};
    for(unsigned i=0;i<5;++i) {
        auto m=make({load(load_functions[i],-4)}); fill(m,1,516); ok(m.memory.store_le(512,4,0x1234FF80));
        ok(step_shader(m,0)); require(m.warps[0].lanes[31].x[3]==expected[i],"Load width/sign");
    }
    for(unsigned f=0;f<3;++f) {
        auto m=make({store(f,-4)});
        for(unsigned lane=0;lane<32;++lane) { ok(initialize_x(m,0,lane,1,516+lane*4)); ok(initialize_x(m,0,lane,2,0x1234FF80+lane)); }
        auto record=step_shader(m,0); ok(record);
        const unsigned bytes=1u<<f;
        const Word mask=bytes==4?0xFFFFFFFFu:(1u<<(8*bytes))-1;
        for(unsigned lane=0;lane<32;++lane) require(m.memory.load_le(512+lane*4,bytes).value()==((0x1234FF80u+lane)&mask),"Store width/value per lane");
        require(record.value().memory_writes.size()==32,"Store trace");
    }
}
void memory_prechecks_and_profiles() {
    auto failed=make({store(2)}); fill(failed,1,512); fill(failed,2,99);
    // Disjoint earlier lanes, invalid final lane: no store may commit.
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(failed,0,lane,1,lane==31?4096:512+lane*4));
    expect_fault(failed,"MemoryRange"); require(failed.memory.load_le(512,4).value()==0,"No partial store");
    auto misaligned=make({load(1)},1); fill(misaligned,1,513);
    expect_fault(misaligned,"DataAlignment"); require(misaligned.warps[0].status==WarpStatus::Ready,"Unknown rule is ModelLimit by default");
    misaligned.config.allow_experimental=true; expect_fault(misaligned,"DataAlignment");
    require(misaligned.warps[0].status==WarpStatus::Faulted,"Experimental alignment profile");
    auto conflict=make({store(2)}); fill(conflict,1,512);
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(conflict,0,lane,2,lane));
    expect_fault(conflict,"StoreConflict"); require(conflict.memory.load_le(512,4).value()==0,"Conflict not submitted by default");
    conflict.config.allow_experimental=true; auto record=step_shader(conflict,0); ok(record);
    require(conflict.memory.load_le(512,4).value()==31 && record.value().experimental_profile,"Explicit lane-order store profile");
    auto write_only=make({store(2)},1); ok(write_only.memory.map_region({3072,4,permission(Access::Write)}));
    fill(write_only,1,3072); fill(write_only,2,42); auto stored=step_shader(write_only,0); ok(stored);
    require(stored.value().memory_writes[0].after==42,"Store does not require read permission");
    auto zero=make({load(2,0,0)},1); fill(zero,1,4096); expect_fault(zero,"MemoryRange");
    auto wrap=make({load(4,1)},1); fill(wrap,1,0xFFFFFFFF); ok(step_shader(wrap,0));
    require(wrap.warps[0].lanes[0].x[3]==static_cast<Word>(load(4,1)&0xFF),"RV32 address addition wraps");
}
void branch_vectors() {
    struct Vector {unsigned f; Word a,b; bool taken;};
    const Vector vectors[]={{0,1,1,true},{0,1,2,false},{1,1,2,true},{1,1,1,false},
        {4,0xFFFFFFFF,0,true},{4,1,0,false},{5,0xFFFFFFFF,0,false},{5,0,0xFFFFFFFF,true},
        {6,0xFFFFFFFF,0,false},{6,0,1,true},{7,0xFFFFFFFF,0,true},{7,0,1,false}};
    for(const auto& v:vectors) { auto m=make({branch(v.f,8)}); fill(m,1,v.a); fill(m,2,v.b); ok(step_shader(m,0)); require(m.warps[0].pc==(v.taken?8u:4u),"Branch decision"); }
    auto same=make({branch(0,8)});
    for(unsigned lane=0;lane<32;++lane) { ok(initialize_x(same,0,lane,1,lane)); ok(initialize_x(same,0,lane,2,lane)); }
    ok(step_shader(same,0)); require(same.warps[0].pc==8,"Uniform decisions with varying operands");
    auto mixed=make({branch(0,8)}); fill(mixed,1,1); fill(mixed,2,1); ok(initialize_x(mixed,0,31,2,2)); expect_fault(mixed,"DivergentBranch");
    auto taken=make({branch(0,2)}); fill(taken,1,0); fill(taken,2,0); expect_fault(taken,"InstructionAlignment");
    auto untaken=make({branch(0,2)}); fill(untaken,1,0); fill(untaken,2,1); ok(step_shader(untaken,0));
    auto highest=make({branch(0,8)},0x80000000); ok(initialize_x(highest,0,31,1,3)); ok(initialize_x(highest,0,31,2,3)); ok(step_shader(highest,0));
}
void jump_and_stop() {
    auto direct=make({jal(8,1)}); ok(step_shader(direct,0));
    require(direct.warps[0].pc==8 && direct.warps[0].lanes[31].x[1]==4,"JAL link and target");
    auto indirect=make({jalr(1,1)}); fill(indirect,1,17); ok(step_shader(indirect,0));
    require(indirect.warps[0].pc==16 && indirect.warps[0].lanes[0].x[1]==4,"JALR alias and bit0 clear");
    auto misaligned=make({jalr(1,1)}); fill(misaligned,1,6); expect_fault(misaligned,"InstructionAlignment");
    require(misaligned.warps[0].lanes[0].x[1]==6,"Invalid JALR preserves link");
    auto mixed=make({jalr(3,1)}); fill(mixed,1,8); ok(initialize_x(mixed,0,31,1,12)); expect_fault(mixed,"NonUniformTarget");
    auto outside=make({jal(512,1)}); ok(step_shader(outside,0));
    require(outside.warps[0].lanes[0].x[1]==4,"Access fault occurs after JAL link commits");
    expect_fault(outside,"MemoryPermission");
    // x1=3; loop: x1--; bne x1,x0,loop. No shader termination instruction is needed for a host pause.
    auto loop=make({imm(0,3,1,0),imm(0,0xFFF,1,1),branch(1,-4,1,0)});
    std::ostringstream trace; auto report=run_until_stop(loop,0,12,&trace);
    require(report.reason==StopReason::Paused && report.retired_steps==7 && loop.warps[0].lanes[0].x[1]==0,"Backward loop and stop_pc");
    auto forever=make({jal(0)}); forever.config.max_steps=3;
    auto timed=run_until_stop(forever,0,4); require(timed.reason==StopReason::Timeout && timed.retired_steps==3,"Unreachable stop budget");
    require(run_until_stop(loop,0,2).reason==StopReason::HostError,"Invalid host stop alignment");
}
void masks_and_csr() {
    for(unsigned operation: {2u,3u,6u,7u}) {
        auto m=make({csr(operation)},0x80000000u); ok(step_shader(m,0));
        require(m.warps[0].lanes[31].x[3]==0x80000000u && !m.warps[0].lanes[0].x_defined[3],"Mask CSR active lanes only");
    }
    auto full=make({csr()}); ok(step_shader(full,0)); require(full.warps[0].lanes[0].x[3]==0xFFFFFFFFu,"Full mask CSR");
    auto partial=make({csr()},7); ok(step_shader(partial,0)); require(partial.warps[0].lanes[2].x[3]==7,"Partial mask CSR");
    for(unsigned operation:{1u,5u,2u,3u,6u,7u}) {
        auto m=make({csr(operation,operation==1||operation==5?0:1)}); fill(m,1,0); expect_fault(m,"ReadOnlyCsr");
    }
}
void split_paths() {
    for(unsigned mode=0;mode<3;++mode) {
        auto m=make({split(),imm(0,10,3,0),jal(8),imm(0,20,3,0),join}); fill(m,2,12);
        for(unsigned lane=0;lane<32;++lane) ok(initialize_x(m,0,lane,1,mode==0?1:mode==1?0:(lane%2==0?9:0)));
        ok(step_shader(m,0)); require(m.warps[0].sp==1,"Even uniform split pushes a frame");
        auto report=run_until_stop(m,0,20); require(report.reason==StopReason::Paused,"if/else completes");
        for(unsigned lane=0;lane<32;++lane) require(m.warps[0].lanes[lane].x[3]==(mode==0?10u:mode==1?20u:lane%2==0?10u:20u),"Masked path result");
        require(m.warps[0].sp==0 && m.warps[0].exec==m.warps[0].init_mask,"Parent mask restored");
    }
    auto no_else=make({split(),imm(0,7,3,0),join}); fill(no_else,2,8);
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(no_else,0,lane,1,lane%2==0));
    auto report=run_until_stop(no_else,0,12); require(report.reason==StopReason::Paused && report.retired_steps==4,"No-else uses same join twice");
    require(!no_else.warps[0].lanes[1].x_defined[3],"False no-else path does not write result");
    auto highest=make({split(),join},0x80000000); ok(initialize_x(highest,0,31,1,0)); ok(initialize_x(highest,0,31,2,4));
    ok(step_shader(highest,0)); ok(step_shader(highest,0)); require(highest.warps[0].exec==0x80000000,"split never assumes lane0");
}
void nested_and_depth() {
    auto m=make({split(),split(3,4),imm(0,11,5,0),jal(12),0x13,imm(0,12,5,0),join,jal(8),imm(0,20,5,0),join});
    fill(m,2,32); fill(m,4,20);
    for(unsigned lane=0;lane<32;++lane) { ok(initialize_x(m,0,lane,1,lane<16)); ok(initialize_x(m,0,lane,3,lane%2==0)); }
    auto report=run_until_stop(m,0,40); require(report.reason==StopReason::Paused,"Nested split completes");
    for(unsigned lane=0;lane<32;++lane) require(m.warps[0].lanes[lane].x[5]==(lane>=16?20u:lane%2==0?11u:12u),"Nested output");
    require(m.warps[0].sp==0 && m.warps[0].exec==0xFFFFFFFFu,"Nested mask restored");
    auto depth=make({split(),split(),split(),split(),split(),split(),split(),split(),join,join,join,join,join,join,join,join});
    fill(depth,1,1); fill(depth,2,0);
    for(unsigned i=0;i<8;++i) ok(step_shader(depth,0));
    require(depth.warps[0].sp==8,"Eight uniform frames allowed");
    require(run_until_stop(depth,0,64).reason==StopReason::Paused && depth.warps[0].sp==0,"Eight joins close frames");
    auto overflow=make({split(),split(),split(),split(),split(),split(),split(),split(),split()}); fill(overflow,1,1); fill(overflow,2,0);
    for(unsigned i=0;i<8;++i) ok(step_shader(overflow,0));
    expect_fault(overflow,"StackOverflow");
}
void simt_faults() {
    auto under=make({join}); expect_fault(under,"StackUnderflow");
    auto targets=make({split()}); fill(targets,1,1); fill(targets,2,4); ok(initialize_x(targets,0,31,2,8)); expect_fault(targets,"NonUniformTarget");
    auto alignment=make({split()}); fill(alignment,1,1); fill(alignment,2,2); expect_fault(alignment,"InstructionAlignment");
    auto bad_mask=make({split(),join}); fill(bad_mask,1,1); fill(bad_mask,2,4); ok(step_shader(bad_mask,0));
    bad_mask.warps[0].exec=1; expect_fault(bad_mask,"MaskMismatch");
    auto reserved=make({split(),join}); fill(reserved,1,1); fill(reserved,2,4); ok(step_shader(reserved,0));
    reserved.warps[0].stack[0].phase=static_cast<StackPhase>(99); expect_fault(reserved,"InvalidStackPhase");
    auto second=make({split(),imm(0,10,3,0),jal(8),imm(0,20,3,0),join,0x13,join}); fill(second,2,12);
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(second,0,lane,1,lane%2));
    for(unsigned i=0;i<4;++i) ok(step_shader(second,0)); // now executing false path
    ok(step_shader(second,0)); second.warps[0].pc=24;
    expect_fault(second,"JoinAddressMismatch");
    auto empty=make({split()}); empty.warps[0].exec=0; expect_fault(empty,"InvalidWarpState");
}
void muldiv_vectors() {
    struct Vector {unsigned f;Word a,b,result;};
    const Vector vectors[]={{0,0xFFFFFFFF,2,0xFFFFFFFE},{1,0xFFFFFFFF,2,0xFFFFFFFF},
        {1,0x80000000,0x80000000,0x40000000},{2,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF},
        {2,0x80000000,0xFFFFFFFF,0x80000000},{3,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFE},
        {4,0xFFFFFFF9,3,0xFFFFFFFE},{4,0x80000000,0xFFFFFFFF,0x80000000},{4,5,0,0xFFFFFFFF},
        {5,0xFFFFFFFF,2,0x7FFFFFFF},{5,5,0,0xFFFFFFFF},{6,0xFFFFFFF9,3,0xFFFFFFFF},
        {6,0x80000000,0xFFFFFFFF,0},{6,0x80000000,0,0x80000000},{7,0xFFFFFFFF,2,1},{7,5,0,5}};
    for(const auto& v:vectors) { auto m=make({reg(v.f,1)}); fill(m,1,v.a); fill(m,2,v.b); ok(step_shader(m,0)); require(m.warps[0].lanes[31].x[3]==v.result,"M extension result"); }
}
void exhaustive_small_masks() {
    // Exhaust all nonempty entry sets and predicate patterns in four lanes of a 32-lane warp.
    for(Mask entry=1;entry<16;++entry) for(Mask predicates=0;predicates<16;++predicates) {
        auto m=make({split(),imm(0,10,3,0),jal(8),imm(0,20,3,0),join},entry);
        for(unsigned lane=0;lane<4;++lane) if(entry&(1u<<lane)) {
            ok(initialize_x(m,0,lane,1,(predicates&(1u<<lane))?5:0));
            ok(initialize_x(m,0,lane,2,12));
        }
        auto report=run_until_stop(m,0,20);
        require(report.reason==StopReason::Paused && m.warps[0].exec==entry && m.warps[0].sp==0,"Exhaustive split mask recovery");
        for(unsigned lane=0;lane<32;++lane) {
            const auto& state=m.warps[0].lanes[lane];
            if(entry&(1u<<lane)) require(state.x[3]==((predicates&(1u<<lane))?10u:20u),"Exhaustive path value");
            else require(!state.x_defined[3],"Exhaustive inactive preservation");
        }
    }
}
void code_bounds_and_load_atomicity() {
    auto m=make({jal(256,1)});
    const std::array<std::uint8_t,4> other={0x13,0,0,0};
    ok(m.memory.load_image(256,other,permission(Access::Execute)|permission(Access::Read)));
    ok(step_shader(m,0)); expect_fault(m,"ShaderCodeRange");
    auto explicit_ranges=make({jal(256,1)});
    ok(explicit_ranges.memory.load_image(256,other,permission(Access::Execute)|permission(Access::Read)));
    auto launched=create_warp(explicit_ranges,{0,1,{{0,256},{256,4}}}); ok(launched);
    ok(step_shader(explicit_ranges,launched.value().value()));
    require(explicit_ranges.warps[1].pc==256,"Explicit shader code ranges");
    auto bad=make({load(2)}); fill(bad,1,512); fill(bad,3,77); ok(initialize_x(bad,0,31,1,4096));
    expect_fault(bad,"MemoryRange"); require(bad.warps[0].lanes[0].x[3]==77,"No partially committed load");
    auto first=make({split(),join}); fill(first,1,1); fill(first,2,4); ok(step_shader(first,0));
    first.warps[0].stack[0].phase=StackPhase::First;
    first.warps[0].stack[0].else_mask=2; expect_fault(first,"MaskMismatch");
    auto second=make({join}); second.warps[0].sp=1;
    second.warps[0].stack[0]={0xFFFFFFFFu,2,0,0,StackPhase::Second};
    expect_fault(second,"MaskMismatch");
}
void trace_control_and_memory() {
    auto m=make({split(),csr(),join}); fill(m,2,8);
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(m,0,lane,1,lane%2==0));
    auto split_record=step_shader(m,0); ok(split_record);
    require(split_record.value().exec_before==0xFFFFFFFF && split_record.value().exec_after==0x55555555 && split_record.value().sp_after==1,"Control trace transitions");
    ok(step_shader(m,0)); require(m.warps[0].lanes[0].x[3]==0x55555555,"CSR reads current EXEC not INIT_MASK");
    std::ostringstream trace; write_trace(trace,split_record.value());
    require(trace.str().find("\"op\":\"gpu.split\"")!=std::string::npos,"SIMT trace op");
}
}
int main() {
    struct Test{const char* name;void(*run)();};
    const Test tests[]={{"B decoder contracts",decode_contracts},{"B1 ALU",alu_vectors},
        {"B2 widths and per-lane stores",load_store_widths},{"B2 prechecks and unresolved policies",memory_prechecks_and_profiles},
        {"B3 branch decisions",branch_vectors},{"B3 jumps and host stop",jump_and_stop},
        {"B4 masks and read-only CSR",masks_and_csr},{"B5 paths",split_paths},
        {"B6 nesting and depth",nested_and_depth},{"B6 faults",simt_faults},
        {"B7 multiplication and division",muldiv_vectors},{"B exhaustive split masks (240 cases)",exhaustive_small_masks},{"B code bounds and load atomicity",code_bounds_and_load_atomicity},{"B control trace",trace_control_and_memory}};
    unsigned failed=0;
    for(const auto& test:tests){try{test.run();std::cout<<"PASS "<<test.name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<test.name<<": "<<e.what()<<'\n';}}
    return failed?1:0;
}
