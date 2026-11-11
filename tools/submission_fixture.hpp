#pragma once
#include "model/submission.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Verification fixtures only. All shader computation runs from actual machine
// words; these helpers only prepare memory, the existing descriptor and metadata.
namespace animus::fixtures {
inline constexpr Addr kCommands=0x100, kDescriptor=0x1000, kVs=0x2000, kFs=0x2400;
inline constexpr Addr kVertices=0x3000, kUniform=0x5000, kTexture=0x5400;
inline constexpr Addr kColor=0x6000, kDepth=0x6400, kScratch=0x7000, kCompletion=0x7400;
inline constexpr Word kBlack=0xFF000000, kRed=0xFF0000FF;
template<class T> void checked(const Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().code+": "+result.error().message);
}
inline void put_words(Machine& machine,Addr address,std::span<const Word> words) {
    for (std::size_t i=0;i<words.size();++i) checked(machine.memory.store_le(address+static_cast<Word>(i*4),4,words[i]));
}
inline void put_words(Machine& machine,Addr address,std::initializer_list<Word> words) { put_words(machine,address,std::span(words.begin(),words.size())); }
inline void put_float(Machine& machine,Addr address,float value) { checked(machine.memory.store_le(address,4,std::bit_cast<Word>(value))); }
inline std::array<Word,32> descriptor() {
    std::array<Word,32> w{};
    w[0]=1; w[1]=3; w[2]=kColor; w[3]=kDepth; w[4]=8; w[5]=8; w[6]=40; w[7]=40;
    w[8]=kVs; w[9]=kFs; w[10]=kVertices; w[11]=16; w[12]=kUniform;
    w[20]=8; w[21]=8; w[24]=8; w[25]=8; return w;
}
inline void commands(Machine& machine,std::span<const Command> commands) {
    std::vector<Word> words;
    for (const auto& command : commands) words.insert(words.end(),{command.opcode,command.a,command.b,command.c});
    put_words(machine,kCommands,words);
    checked(mmio_write(machine,0,kCommands)); checked(mmio_write(machine,4,static_cast<Word>(words.size()*4)));
}
inline void commands(Machine& machine,std::initializer_list<Command> list) { commands(machine,std::span(list.begin(),list.size())); }
inline void standard_commands(Machine& machine,Word vertex_count=6) {
    commands(machine,{{1,kDescriptor,0,0},{2,3,kBlack,0x3F800000},{3,0,vertex_count,0},{4,kCompletion,0x12345678,0},{255,0,0,0}});
}
inline Machine make() {
    ModelConfig config; config.memory_bytes=32768; config.raster_profile=RasterProfile::ReferenceV1;
    config.fp_state_policy=FpStatePolicy::PerLaneExperimental; config.sfu_profile=SfuProfile::HostMathV1;
    auto created=create_machine(config); checked(created); auto machine=std::move(created.value());
    checked(machine.memory.map_region({0,32768,7}));
    const std::array<Resource,11> resources={{{ResourceRole::Command,kCommands,2048,1},
        {ResourceRole::Descriptor,kDescriptor,128,1},{ResourceRole::Shader,kVs,256,5},{ResourceRole::Shader,kFs,1024,5},
        {ResourceRole::Vertex,kVertices,4096,1},{ResourceRole::Uniform,kUniform,256,1},{ResourceRole::Texture,kTexture,256,1},
        {ResourceRole::Color,kColor,1024,3},{ResourceRole::Depth,kDepth,1024,3},{ResourceRole::Scratch,kScratch,1024,3},
        {ResourceRole::Completion,kCompletion,16,3}}};
    checked(install_resources(machine,resources));
    // VS: get vertex address, load position into f0..f3, end. FS: constant red, end.
    put_words(machine,kVs,{0x001000AB,0x0000A007,0x0040A087,0x0080A107,0x00C0A187,0x0000302B});
    put_words(machine,kFs,{0xFF000537,0x0FF50513,0x0000302B});
    const std::array<std::array<float,4>,6> vertices={{{-0.5f,-0.5f,0.5f,1.0f},{0.5f,-0.5f,0.5f,1.0f},{-0.5f,0.5f,0.5f,1.0f},
        {0.5f,-0.5f,0.5f,1.0f},{0.5f,0.5f,0.5f,1.0f},{-0.5f,0.5f,0.5f,1.0f}}};
    for (unsigned i=0;i<6;++i) for (unsigned k=0;k<4;++k) put_float(machine,kVertices+i*16+k*4,vertices[i][k]);
    const auto state=descriptor(); put_words(machine,kDescriptor,state);
    standard_commands(machine); return machine;
}
inline RunReport submit(Machine& machine,std::ostream* trace=nullptr) { checked(mmio_write(machine,8,1)); return run_submission(machine,trace); }
inline Word pixel(const Machine& machine,unsigned x,unsigned y,bool depth=false) {
    auto value=machine.memory.load_le((depth?kDepth:kColor)+y*40+x*4,4); checked(value); return value.value();
}
} // namespace animus::fixtures
