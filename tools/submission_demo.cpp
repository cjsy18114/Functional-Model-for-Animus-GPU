#include "submission_fixture.hpp"
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
void write_word(std::ostream& stream,animus::Word word) {
    for (unsigned byte=0;byte<4;++byte) stream.put(static_cast<char>(word>>(8*byte)));
}
}
int main(int argc,char** argv) {
    using namespace animus;
    namespace f=animus::fixtures;
    try {
        std::filesystem::path vertex_file,output="build/e_framebuffer.bin";
        Word flags=3;
        for (int i=1;i<argc;++i) {
            const std::string_view option=argv[i];
            if (i+1==argc) throw std::runtime_error("Option requires a value.");
            if (option=="--vertices") vertex_file=argv[++i];
            else if (option=="--output") output=argv[++i];
            else if (option=="--flags") {
                const std::string_view value=argv[++i];
                auto parsed=std::from_chars(value.data(),value.data()+value.size(),flags);
                if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || flags>7) throw std::runtime_error("Flags must be 0..7.");
            } else throw std::runtime_error("Usage: animus_submission_demo [--vertices fp32_xyzw.bin] [--output rgba.bin] [--flags 0..7]");
        }
        auto machine=f::make(); auto descriptor=f::descriptor(); descriptor[1]=flags;
        f::put_words(machine,f::kDescriptor,descriptor);
        Word count=6;
        if (!vertex_file.empty()) {
            std::ifstream input(vertex_file,std::ios::binary|std::ios::ate);
            if (!input) throw std::runtime_error("Cannot open vertex file.");
            const auto size=input.tellg();
            if (size<0 || size>4096 || size%48!=0) throw std::runtime_error("Vertex file must contain complete triangles of four FP32 words per vertex, at most 4096 bytes.");
            input.seekg(0); count=static_cast<Word>(size/16);
            for (std::streamoff byte=0;byte<size;byte+=4) {
                Word word=0;
                for (unsigned k=0;k<4;++k) { const int value=input.get(); if (value<0) throw std::runtime_error("Vertex file read failed."); word|=Word{static_cast<unsigned>(value)}<<(8*k); }
                f::checked(machine.memory.store_le(f::kVertices+static_cast<Addr>(byte),4,word));
            }
        }
        f::standard_commands(machine,count);
        const auto report=f::submit(machine);
        if (report.reason!=StopReason::Completed) { if (report.issue) write_issue(std::cerr,*report.issue); return 1; }
        std::ofstream colors(output,std::ios::binary),depth(output.string()+".depth.bin",std::ios::binary);
        if (!colors || !depth) throw std::runtime_error("Cannot open output paths (parent directory must exist).");
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
            write_word(colors,f::pixel(machine,x,y)); write_word(depth,f::pixel(machine,x,y,true));
        }
        colors.flush(); depth.flush(); if (!colors || !depth) throw std::runtime_error("Framebuffer output write failed.");
        const auto& s=machine.submission;
        std::cout<<"{\"status\":"<<static_cast<Word>(s.status)<<",\"error_code\":"<<s.error_code<<",\"commands\":"<<s.completed_commands
                 <<",\"primitives\":"<<s.completed_primitives<<",\"fragments\":"<<s.merged_fragments<<",\"shader_attempts\":"<<s.shader_attempts
                 <<",\"work_items\":"<<s.work_items<<",\"fence\":"<<machine.memory.load_le(f::kCompletion,4).value()
                 <<",\"width\":8,\"height\":8,\"raster_profile\":\"reference-v1\",\"fp_state_policy\":\"per-lane-experimental\",\"sfu_profile\":\"host-math-v1\"}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 2; }
}
