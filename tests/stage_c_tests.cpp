#include "model/model.hpp"
#include "model/execution.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
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
std::set<Op> successful_ops, fault_ops;
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class T> void ok(const Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}
Result<StepRecord> step(Machine& machine, WarpId id=0) {
    auto result = step_shader(machine,id);
    if(result) successful_ops.insert(result.value().op);
    else if(result.error().raw) { auto decoded=decode(*result.error().raw); if(decoded) fault_ops.insert(decoded.value().op); }
    return result;
}
Word fp(unsigned f7, unsigned f3=0, unsigned rd=3, unsigned a=1, unsigned b=2) {
    return (f7<<25)|(b<<20)|(a<<15)|(f3<<12)|(rd<<7)|0x53;
}
Word fma(unsigned opcode, unsigned rm=0) { return (4u<<27)|(2u<<20)|(1u<<15)|(rm<<12)|(3u<<7)|opcode; }
Word csr(unsigned f3, unsigned address=3, unsigned source=1, unsigned rd=3) {
    return (address<<20)|(source<<15)|(f3<<12)|(rd<<7)|0x73;
}
Word sfu(unsigned index, unsigned rd=3) { return ((32u+index)<<25)|(1u<<15)|(rd<<7)|0x0B; }
Word addi(unsigned rd, unsigned a, unsigned value) { return ((value&0xFFF)<<20)|(a<<15)|(rd<<7)|0x13; }
Word float_bits(float value) { return std::bit_cast<Word>(value); }
Machine make(std::initializer_list<Word> program, Mask mask=0xFFFFFFFFu, bool experimental=true) {
    ModelConfig config; config.memory_bytes=8192;
    // Explicit test profile; this does not assert that the team approved a per-lane hardware fcsr.
    if(experimental) { config.fp_state_policy=FpStatePolicy::PerLaneExperimental; config.sfu_profile=SfuProfile::HostMathV1; }
    auto created=create_machine(config); ok(created); auto machine=std::move(created.value());
    std::vector<std::uint8_t> bytes(512,0);
    unsigned offset=0;
    for(Word value:program) for(unsigned i=0;i<4;++i) bytes.at(offset++)=static_cast<std::uint8_t>(value>>(8*i));
    ok(machine.memory.load_image(0,bytes,permission(Access::Read)|permission(Access::Execute)));
    ok(machine.memory.map_region({1024,4096,permission(Access::Read)|permission(Access::Write)}));
    auto launched=create_warp(machine,{0,mask}); ok(launched); return machine;
}
void fill_x(Machine& machine, unsigned reg, Word value, WarpId id=0) {
    for(unsigned lane=0;lane<32;++lane) ok(initialize_x(machine,id,lane,reg,value));
}
void fill_f(Machine& machine, unsigned reg, Word value, WarpId id=0) {
    for(unsigned lane=0;lane<32;++lane) ok(initialize_f(machine,id,lane,reg,value));
}
void expect_issue(Machine& machine, const std::string& code, WarpId id=0) {
    const auto before=machine.warps[id]; const auto retired=machine.retired_steps;
    auto result=step(machine,id);
    require(!result && result.error().code==code,"Expected "+code);
    const auto& after=machine.warps[id];
    require(after.pc==before.pc && after.exec==before.exec && after.sp==before.sp && machine.retired_steps==retired,"Failed instruction must not commit control/retirement");
    for(unsigned lane=0;lane<32;++lane) require(after.lanes[lane].x==before.lanes[lane].x && after.lanes[lane].f_bits==before.lanes[lane].f_bits && after.lanes[lane].fcsr==before.lanes[lane].fcsr,"Failed instruction must not commit data/CSR");
}
void decoder_contracts() {
    struct V{Word raw; Op op;};
    const V vectors[]={
        {0x002081D3,Op::FloatAdd},{0x082081D3,Op::FloatSubtract},{0x102081D3,Op::FloatMultiply},
        {0x182081D3,Op::FloatDivide},{0x580081D3,Op::FloatSqrt},{0x202081D3,Op::FloatSignInject},
        {0x202091D3,Op::FloatSignInvert},{0x2020A1D3,Op::FloatSignXor},{0x282081D3,Op::FloatMin},
        {0x282091D3,Op::FloatMax},{0xA02081D3,Op::FloatLessEqual},{0xA02091D3,Op::FloatLess},
        {0xA020A1D3,Op::FloatEqual},{0xE00091D3,Op::FloatClassify},{0xC00081D3,Op::FloatToInt},
        {0xC01081D3,Op::FloatToUnsigned},{0xD00081D3,Op::IntToFloat},{0xD01081D3,Op::UnsignedToFloat},
        {0xE00081D3,Op::MoveFloatToInt},{0xF00081D3,Op::MoveIntToFloat},
        {0x202081C3,Op::FloatMultiplyAdd},{0x202081C7,Op::FloatMultiplySubtract},
        {0x202081CB,Op::FloatNegMultiplySubtract},{0x202081CF,Op::FloatNegMultiplyAdd},
        {0x0000A187,Op::LoadFloat},{0x0020A027,Op::StoreFloat},{0x00000073,Op::EnvironmentCall},
        {0x00100073,Op::Breakpoint},{0x0FF0000F,Op::Fence}};
    for(const auto& v:vectors) {auto decoded=decode(v.raw);ok(decoded);require(decoded.value().op==v.op,"Fixed C encoding");}
    for(Word invalid:{fp(0,5),fp(0,6),fp(44,0,3,1,1),fp(16,3),fp(20,2),fp(80,3),fp(96,0,3,1,2),
                      fp(104,0,3,1,2),fp(112,2,3,1,0),fp(120,1,3,1,0),fma(0x43)|0x02000000u,sfu(0)|0x00100000u,
                      0x000000F3u,0x00008073u}) {
        auto decoded=decode(invalid);require(!decoded && decoded.error().kind==IssueKind::ArchitecturalFault,"Malformed C encoding rejected");
    }
    require(!decode(0x0000100F),"FENCE.I still unsupported");
}
void scheduler_and_reuse() {
    auto m=make({addi(1,0,1),addi(2,1,1),0,0,addi(1,0,2),addi(2,1,2),0,0,
                 addi(1,0,3),addi(2,1,3),0,0,addi(1,0,4),addi(2,1,4)});
    for(unsigned id=1;id<4;++id) {auto launched=create_warp(m,{id*16,0xFFFFFFFFu});ok(launched);}
    const std::array<WarpStop,4> stops={{{0,8},{1,24},{2,40},{3,56}}};
    std::ostringstream trace;auto report=run_warps_until_stops(m,stops,&trace);
    require(report.reason==StopReason::Paused && report.retired_steps==8,"Four-warp host schedule");
    std::istringstream lines(trace.str());std::string line;
    for(unsigned id=0;id<4;++id){std::getline(lines,line);require(line.find("\"warp\":"+std::to_string(id))!=std::string::npos,"Deterministic round-robin order");}
    for(unsigned id=0;id<4;++id) {
        require(m.warps[id].lanes[31].x[2]==2*(id+1),"Separate register files");
        m.warps[id].lanes[0].fcsr=static_cast<std::uint8_t>(id);
    }
    require(!release_warp(m,0),"Paused Ready warp cannot be silently released");
    const auto invocation=m.warps[0].invocation_id;
    ok(finish_test_warp(m,0,8));auto snapshot=release_warp(m,0);ok(snapshot);
    auto launched=create_warp(m,{0,1});ok(launched);
    require(launched.value()==0 && m.warps[0].invocation_id!=invocation,"New identity on slot reuse");
    require(!m.warps[0].lanes[0].x_defined[1] && !m.warps[0].lanes[0].f_defined[0] && m.warps[0].sp==0 && m.warps[0].lanes[0].fcsr==0,"Slot reuse resets all data");
    require(snapshot.value().lanes[31].x[2]==2,"Snapshot survives slot reuse");
    m.warps[0].status=WarpStatus::Waiting;
    require(choose_ready(m)==1,"Waiting warp skipped");
    const std::array<WarpStop,1> waiting={{{0,4}}};
    require(run_warps_until_stops(m,waiting).reason==StopReason::SuspectedDeadlock,"No-ready diagnosis");
    require(!initialize_f(m,9,0,0,0),"Invalid FP initialization rejected");
}
void multiwarp_memory_and_stack() {
    const Word sw=(2u<<20)|(1u<<15)|(2u<<12)|0x23;
    auto m=make({sw});
    for(unsigned id=1;id<4;++id){auto launch=create_warp(m,{0,0xFFFFFFFFu});ok(launch);}
    for(unsigned id=0;id<4;++id)for(unsigned lane=0;lane<32;++lane){
        ok(initialize_x(m,id,lane,1,1024+id*128+lane*4));ok(initialize_x(m,id,lane,2,id*100+lane));}
    const std::array<WarpStop,4> stops={{{0,4},{1,4},{2,4},{3,4}}};
    require(run_warps_until_stops(m,stops).reason==StopReason::Paused,"Shared memory, independent addresses");
    for(unsigned id=0;id<4;++id)for(unsigned lane=0;lane<32;++lane)
        require(m.memory.load_le(1024+id*128+lane*4,4).value()==id*100+lane,"Cross-warp data not mixed");
    auto stack=make({0x0420800B,0x0600000B},1);
    const Mask masks[]={1,3,7,0x80000000u};
    for(unsigned id=1;id<4;++id){auto launch=create_warp(stack,{0,masks[id]});ok(launch);}
    for(unsigned id=0;id<4;++id){fill_x(stack,1,1,id);fill_x(stack,2,4,id);}
    require(run_warps_until_stops(stack,stops).reason==StopReason::Paused,"Four split contexts");
    for(unsigned id=0;id<4;++id)require(stack.warps[id].sp==1 && stack.warps[id].stack[0].entry_mask==masks[id],"Separate stack frames");
    require(!finish_test_warp(stack,0,4),"Unreconverged host finish rejected");
    const std::array<WarpStop,4> ends={{{0,8},{1,8},{2,8},{3,8}}};
    require(run_warps_until_stops(stack,ends).reason==StopReason::Paused,"Four joins");
    for(unsigned id=0;id<4;++id){require(stack.warps[id].exec==masks[id] && stack.warps[id].sp==0,"Separate restored masks");ok(finish_test_warp(stack,id,8));ok(release_warp(stack,id));}
    const std::array<WarpStop,2> duplicates={{{0,8},{0,8}}};
    require(run_warps_until_stops(stack,duplicates).reason==StopReason::HostError,"Bad batch rejected");
}
void float_memory_and_moves() {
    const Word flw=(1u<<15)|(2u<<12)|0x07; // flw f0,0(x1)
    const Word fsw=(4u<<25)|(1u<<15)|(2u<<12)|0x27; // fsw f0,128(x1)
    auto m=make({flw,fsw},0xFFFFFFFFu,false);
    for(unsigned lane=0;lane<32;++lane){ok(initialize_x(m,0,lane,1,1024+lane*4));ok(m.memory.store_le(1024+lane*4,4,0x7F800001u+lane));}
    ok(step(m));ok(step(m));
    for(unsigned lane=0;lane<32;++lane)require(m.memory.load_le(1152+lane*4,4).value()==0x7F800001u+lane && m.warps[0].lanes[lane].fcsr==0,"FLW/FSW preserve payload, f0 is writable");
    auto moves=make({fp(120,0,0,1,0),fp(112,0,3,0,0)},0x80000000u,false);
    ok(initialize_x(moves,0,31,1,0xFFC01234));ok(step(moves));ok(step(moves));
    require(moves.warps[0].lanes[31].x[3]==0xFFC01234 && !moves.warps[0].lanes[0].f_defined[0],"FMV is bit-preserving and masked");
    auto undefined=make({fp(0)});fill_f(undefined,1,0x3F800000);
    for(unsigned lane=0;lane<31;++lane)ok(initialize_f(undefined,0,lane,2,0x40000000));
    expect_issue(undefined,"UndefinedFloatRegister");
    auto no_policy=make({fp(0)},1,false);fill_f(no_policy,1,0x3F800000);fill_f(no_policy,2,0x40000000);
    expect_issue(no_policy,"FpStatePolicyRequired");
}
void basic_float_arithmetic() {
    struct V{unsigned f7;Word a,b,expected;};
    const V vectors[]={{0,0x3F800000,0x40000000,0x40400000},{4,0x40800000,0x3F800000,0x40400000},
        {8,0x40000000,0x40400000,0x40C00000},{12,0x40C00000,0x40400000,0x40000000},{44,0x40800000,0,0x40000000}};
    for(const auto& v:vectors){auto m=make({fp(v.f7,0,3,1,v.f7==44?0:2)});fill_f(m,1,v.a);fill_f(m,2,v.b);ok(step(m));require(m.warps[0].lanes[31].f_bits[3]==v.expected,"FP basic arithmetic");}
    auto varying=make({fp(0)});
    for(unsigned lane=0;lane<32;++lane){ok(initialize_f(varying,0,lane,1,float_bits(float(lane))));ok(initialize_f(varying,0,lane,2,0x3F800000));}
    ok(step(varying));for(unsigned lane=0;lane<32;++lane)require(varying.warps[0].lanes[lane].f_bits[3]==float_bits(float(lane+1)),"Lane-specific float inputs");
}
void rounding_and_fma() {
    const Word positive[]={0x3F800000,0x3F800000,0x3F800000,0x3F800001,0x3F800001};
    const Word negative[]={0xBF800000,0xBF800000,0xBF800001,0xBF800000,0xBF800001};
    for(unsigned rm=0;rm<5;++rm)for(unsigned sign=0;sign<2;++sign){
        auto m=make({fp(0,rm)},1);fill_f(m,1,sign?0xBF800000:0x3F800000);fill_f(m,2,sign?0xB3800000:0x33800000);
        ok(step(m));require(m.warps[0].lanes[0].f_bits[3]==(sign?negative[rm]:positive[rm]) && m.warps[0].lanes[0].fcsr==1,"All five rounding modes");}
    auto dynamic=make({fp(0,7)});fill_f(dynamic,1,0x3F800000);fill_f(dynamic,2,0x33800000);
    for(unsigned lane=0;lane<32;++lane)dynamic.warps[0].lanes[lane].fcsr=static_cast<std::uint8_t>((lane%5)<<5);
    ok(step(dynamic));for(unsigned lane=0;lane<32;++lane)require(dynamic.warps[0].lanes[lane].f_bits[3]==positive[lane%5],"Per-lane DYN rounding");
    auto reserved=make({fp(0,7)});fill_f(reserved,1,0);fill_f(reserved,2,0);reserved.warps[0].lanes[31].fcsr=0xE0;
    expect_issue(reserved,"InvalidRoundingMode");
    const unsigned ops[]={0x43,0x47,0x4B,0x4F};const Word results[]={0x41200000,0x40000000,0xC0000000,0xC1200000};
    for(unsigned i=0;i<4;++i){auto m=make({fma(ops[i])},1);fill_f(m,1,0x40000000);fill_f(m,2,0x40400000);fill_f(m,4,0x40800000);ok(step(m));require(m.warps[0].lanes[0].f_bits[3]==results[i],"Four FMA signs");}
    auto fused=make({fma(0x47)},1);fill_f(fused,1,0x3F800001);fill_f(fused,2,0x3F7FFFFE);fill_f(fused,4,0x3F800000);
    ok(step(fused));require(fused.warps[0].lanes[0].f_bits[3]==0xA8800000,"FMA rounds once, preserving -2^-46");
}
void nan_zero_flags() {
    struct V{Word instruction,a,b,expected,flags;bool integer;};
    const V vectors[]={
        {fp(12),0x3F800000,0,0x7F800000,8,false},{fp(12),0,0,0x7FC00000,16,false},
        {fp(8),0x7F7FFFFF,0x40000000,0x7F800000,5,false},{fp(8),1,0x3F000000,0,3,false},
        {fp(20),0x7F800001,0x3F800000,0x3F800000,16,false},{fp(20,1),0x7FC01234,0x3F800000,0x3F800000,0,false},
        {fp(20),0x7FC01234,0x7FC05555,0x7FC00000,0,false},{fp(20),0,0x80000000,0x80000000,0,false},
        {fp(20,1),0,0x80000000,0,0,false},{fp(80,2),0x7FC01234,0x3F800000,0,0,true},
        {fp(80,2),0x7F800001,0x3F800000,0,16,true},{fp(80,1),0x7FC01234,0x3F800000,0,16,true},
        {fp(80,0),0x80000000,0,1,0,true},{fp(16),0x7F800001,0x80000000,0xFF800001,0,false},
        {fp(16,1),0x3F800000,0,0xBF800000,0,false},{fp(16,2),0xBF800000,0x80000000,0x3F800000,0,false}};
    for(const auto& v:vectors){auto m=make({v.instruction},1);fill_f(m,1,v.a);fill_f(m,2,v.b);ok(step(m));const auto& lane=m.warps[0].lanes[0];require((v.integer?lane.x[3]:lane.f_bits[3])==v.expected && lane.fcsr==v.flags,"NaN/zero/flags rule");}
    const Word classes[]={0xFF800000,0xBF800000,0x80000001,0x80000000,0,1,0x3F800000,0x7F800000,0x7F800001,0x7FC00000};
    for(unsigned index=0;index<10;++index){auto m=make({fp(112,1,3,1,0)},1,false);fill_f(m,1,classes[index]);ok(step(m));require(m.warps[0].lanes[0].x[3]==(1u<<index),"All FCLASS classes");}
    auto sticky=make({fp(12),fp(0),csr(2,1,0)},1);fill_f(sticky,1,0x3F800000);fill_f(sticky,2,0);ok(step(sticky));fill_f(sticky,2,0x33800000);ok(step(sticky));ok(step(sticky));
    require(sticky.warps[0].lanes[0].x[3]==9,"fflags accumulates DZ and NX");
}
void conversions() {
    struct V{Word instruction,input,expected,flags;bool to_float;};
    const V vectors[]={
        {fp(96,0,3,1,0),0xBFE00000,0xFFFFFFFE,1,false},{fp(96,1,3,1,0),0xBFE00000,0xFFFFFFFF,1,false},
        {fp(96,0,3,1,0),0x7FC00000,0x7FFFFFFF,16,false},{fp(96,0,3,1,0),0x4F000000,0x7FFFFFFF,16,false},
        {fp(96,0,3,1,0),0xFF800000,0x80000000,16,false},{fp(96,0,3,1,1),0xBF800000,0,16,false},
        {fp(96,0,3,1,1),0x7FC00000,0xFFFFFFFF,16,false},{fp(96,1,3,1,1),0xBE800000,0,1,false},
        {fp(104,0,3,1,0),16777217,0x4B800000,1,true},{fp(104,0,3,1,1),0xFFFFFFFF,0x4F800000,1,true},
        {fp(104,0,3,1,0),0x80000000,0xCF000000,0,true}};
    for(const auto& v:vectors){auto m=make({v.instruction},1);if(v.to_float)fill_x(m,1,v.input);else fill_f(m,1,v.input);ok(step(m));const auto& lane=m.warps[0].lanes[0];require((v.to_float?lane.f_bits[3]:lane.x[3])==v.expected && lane.fcsr==v.flags,"Conversion/clipping rule");}
}
void csr_rules() {
    for(unsigned operation:{1u,2u,3u,5u,6u,7u}){
        auto m=make({csr(operation)},1);m.warps[0].lanes[0].fcsr=0xA5;fill_x(m,1,7);
        const Word source=operation>=5?1:7;
        const Word expected=(operation==1||operation==5)?source:(operation==2||operation==6)?0xA5|source:0xA5&~source;
        ok(step(m));require(m.warps[0].lanes[0].x[3]==0xA5 && m.warps[0].lanes[0].fcsr==(expected&255),"All six CSR forms");
    }
    auto fields=make({csr(2,1,0),csr(2,2,0),csr(1,1,1,0),csr(1,2,1,0)},1);
    fields.warps[0].lanes[0].fcsr=0xA5;fill_x(fields,1,0xFFFFFFFF);ok(step(fields));require(fields.warps[0].lanes[0].x[3]==5,"fflags field");
    ok(step(fields));require(fields.warps[0].lanes[0].x[3]==5,"frm field");
    auto write_flags=step(fields);ok(write_flags);require(write_flags.value().register_writes.empty() && fields.warps[0].lanes[0].fcsr==0xBF,"CSRRW rd0 suppresses output/read, preserves frm");
    ok(step(fields));require(fields.warps[0].lanes[0].fcsr==0xFF,"frm high-bit mask");
    auto zero_value=make({csr(2)},1);fill_x(zero_value,1,0);auto attempted=step(zero_value);ok(attempted);require(attempted.value().csr_writes.size()==1,"Nonzero source index with zero value is still a write request");
    auto no_write=make({csr(2,3,0)},1);auto suppressed=step(no_write);ok(suppressed);require(suppressed.value().csr_writes.empty(),"Zero source index suppresses CSRRS write");
    auto alias=make({csr(1,3,1,1)},1);fill_x(alias,1,7);alias.warps[0].lanes[0].fcsr=2;ok(step(alias));require(alias.warps[0].lanes[0].x[1]==2 && alias.warps[0].lanes[0].fcsr==7,"CSR source/destination alias");
    auto unknown=make({csr(2,0xC00,0)},1);expect_issue(unknown,"CsrNotImplemented");
    auto strict=make({csr(2,3,0)},1,false);expect_issue(strict,"FpStatePolicyRequired");
}
void fence_and_exceptions() {
    auto m=make({0x0FF0000F,0x8330000F});ok(step(m));ok(step(m));require(m.warps[0].pc==8,"FENCE and FENCE.TSO retire without fake delay");
    auto reserved=make({0xF1208F8F});ok(step(reserved));require(reserved.warps[0].pc==4,"Reserved FENCE fields ignored without reading undefined x1 or writing x31");
    auto call=make({0x00000073},1);expect_issue(call,"EnvironmentCall");require(call.warps[0].status==WarpStatus::Faulted,"ECALL is not successful completion");
    auto breakpoint=make({0x00100073},1);expect_issue(breakpoint,"Breakpoint");
    ok(release_warp(breakpoint,0));
}
void sfu_profiles_and_values() {
    const Word inputs[]={0x40000000,0x40800000,0x40400000,0x41000000,0,0};
    const Word outputs[]={0x3F000000,0x3F000000,0x41000000,0x40400000,0,0x3F800000};
    for(unsigned index=0;index<6;++index){auto m=make({sfu(index)});fill_f(m,1,inputs[index]);for(auto& lane:m.warps[0].lanes)lane.fcsr=31;ok(step(m));for(const auto& lane:m.warps[0].lanes)require(lane.f_bits[3]==outputs[index] && lane.fcsr==31,"SFU full warp and flag preservation");}
    auto disabled=make({sfu(0)},1,false);fill_f(disabled,1,0x40000000);expect_issue(disabled,"SfuProfileRequired");
    struct V{unsigned op;Word input,output;};const V vectors[]={{0,0x80000000,0xFF800000},{0,0x7F800000,0},{1,0xBF800000,0x7FC00000},
        {2,0xFF800000,0},{3,0x80000000,0xFF800000},{4,0x7F800000,0x7FC00000},{5,0x7FC01234,0x7FC00000}};
    for(const auto& v:vectors){auto m=make({sfu(v.op)},1);fill_f(m,1,v.input);ok(step(m));require(m.warps[0].lanes[0].f_bits[3]==v.output,"SFU reference special values");}
    const int previous=std::fegetround();std::fesetround(FE_DOWNWARD);
    auto reference=sfu_eval(Op::SfuSin,0,SfuProfile::HostMathV1);ok(reference);
    require(std::fegetround()==FE_DOWNWARD,"SFU restores host rounding environment");std::fesetround(previous);
}
void fp_trace_and_isolation() {
    auto m=make({fp(12),csr(2,1,0)});fill_f(m,1,0x3F800000);fill_f(m,2,0);
    auto other=create_warp(m,{0,1});ok(other);fill_f(m,1,0x3F800000,1);fill_f(m,2,0x40000000,1);
    auto record=step(m);ok(record);auto second=step(m,1);ok(second);
    require(m.warps[0].lanes[0].fcsr==8 && m.warps[1].lanes[0].fcsr==0,"FP CSR isolated by warp");
    std::ostringstream trace;write_trace(trace,record.value());
    require(trace.str().find("\"file\":\"f\"")!=std::string::npos && trace.str().find("\"csr_writes\":[{")!=std::string::npos && trace.str().find("per-lane-experimental")!=std::string::npos,"FP trace carries file, flags and selected scope");
}
void instruction_coverage() {
    unsigned covered=0;
    for(unsigned index=static_cast<unsigned>(Op::LoadFloat);index<=static_cast<unsigned>(Op::SfuCos);++index){
        const auto op=static_cast<Op>(index);
        const bool covered_op=(op==Op::EnvironmentCall||op==Op::Breakpoint)?fault_ops.contains(op):successful_ops.contains(op);
        require(covered_op,"Missing C instruction test: "+std::string(op_name(op)));++covered;
    }
    std::cout<<"C instruction coverage: "<<covered<<"/"<<covered<<" (ECALL/EBREAK via expected exceptions)\n";
}
}
int main(){
    struct Test{const char* name;void(*run)();};
    const Test tests[]={{"C decoder contracts",decoder_contracts},{"C1 scheduler and slot reuse",scheduler_and_reuse},
        {"C1 shared memory and independent SIMT stacks",multiwarp_memory_and_stack},{"C2 float memory, moves and source checks",float_memory_and_moves},
        {"C2 basic floating arithmetic",basic_float_arithmetic},{"C3 rounding and fused arithmetic",rounding_and_fma},
        {"C3 NaN, zero, classification and flags",nan_zero_flags},{"C3 conversions",conversions},
        {"C4 CSR rules",csr_rules},{"C4 fence and exceptions",fence_and_exceptions},
        {"C5 SFU profiles and values",sfu_profiles_and_values},{"C floating trace and isolation",fp_trace_and_isolation},
        {"C instruction coverage",instruction_coverage}};
    unsigned failed=0;
    for(const auto& test:tests){try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<test.name<<": "<<e.what()<<'\n';}}
    return failed?1:0;
}
