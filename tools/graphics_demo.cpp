#include "model/model.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

// D-stage host fixture only: the actual input loads, sampling and export are
// executed from the supplied machine-code file, not calculated by this driver.
int main(int argc, char** argv) {
    using namespace animus;
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: animus_graphics_demo <fs.bin> [uv.bin: 1..32 little-endian FP32 u/v pairs]\n";
        return 2;
    }
    std::vector<std::uint8_t> uv;
    if (argc == 3) {
        std::ifstream file(std::filesystem::path(argv[2]),std::ios::binary | std::ios::ate);
        const auto size = file ? file.tellg() : std::streampos{-1};
        if (size <= 0 || size > 256 || size % 8 != 0) { std::cerr << "UV file must contain 1..32 complete pairs.\n"; return 2; }
        uv.resize(static_cast<std::size_t>(size)); file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(uv.data()),static_cast<std::streamsize>(uv.size()))) return 2;
    } else {
        const std::array<Word,8> corners = {0,0,0x3F800000,0,0,0x3F800000,0x3F800000,0x3F800000};
        for (const auto word : corners) for (unsigned byte = 0; byte < 4; ++byte) uv.push_back(static_cast<std::uint8_t>(word >> (8*byte)));
    }
    ModelConfig config; config.memory_bytes = 8192;
    auto created = create_machine(config);
    if (!created) { write_issue(std::cerr,created.error()); return 1; }
    auto& machine = created.value();
    auto loaded = load_binary(machine,{std::filesystem::path(argv[1]),0,0});
    if (!loaded) { write_issue(std::cerr,loaded.error()); return 1; }
    auto inputs = machine.memory.load_image(1024,uv,permission(Access::Read));
    if (!inputs) { write_issue(std::cerr,inputs.error()); return 1; }
    const std::array<Word,8> texels = {0xFF0000FF,0xFF00FF00,0xFFFF0000,0xDEADBEEF,
                                     0xFFFFFFFF,0xFF000000,0xFFFF00FF,0xDEADBEEF};
    std::vector<std::uint8_t> texture;
    for (const auto word : texels) for (unsigned byte = 0; byte < 4; ++byte) texture.push_back(static_cast<std::uint8_t>(word >> (8*byte)));
    auto image = machine.memory.load_image(2048,texture,permission(Access::Read));
    if (!image) { write_issue(std::cerr,image.error()); return 1; }
    const std::array<std::uint8_t,12> interpolation{};
    auto record = machine.memory.load_image(3072,interpolation,permission(Access::Read));
    if (!record) { write_issue(std::cerr,record.error()); return 1; }
    const unsigned count = static_cast<unsigned>(uv.size()/8);
    LaunchInfo launch; launch.init_mask = width_mask(count); launch.dispatch.stage = ShaderStage::Fragment;
    launch.dispatch.graphics = {2048,3,2,16};
    for (unsigned lane = 0; lane < count; ++lane) {
        auto& input = launch.dispatch.inputs[lane];
        input.uniform_base = 1024; input.pixel_x = lane; input.pixel_y = 7;
        input.depth_bits = 0x3F000000; input.primitive_id = 3; input.prim_data_addr = 3072;
    }
    auto warp = create_warp(machine,launch);
    if (!warp || !warp.value()) { if (!warp) write_issue(std::cerr,warp.error()); return 1; }
    const std::array<WarpId,1> ids = {*warp.value()};
    auto report = run_shader_batch(machine,ids,&std::cout);
    if (report.reason != StopReason::Completed) { if (report.issue) write_issue(std::cerr,*report.issue); return 1; }
    auto snapshot = release_warp(machine,ids[0]);
    if (!snapshot) { write_issue(std::cerr,snapshot.error()); return 1; }
    std::cerr << "Completed " << snapshot.value().completed_outputs->fragments.size()
              << " fragment outputs via gfx.end in " << report.retired_steps << " logical instructions.\n";
    return 0;
}
