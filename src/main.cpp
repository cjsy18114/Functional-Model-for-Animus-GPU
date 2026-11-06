#include "model/model.hpp"
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

int main(int argc, char** argv) {
    using namespace animus;
    if (argc != 3) {
        std::cerr << "Usage: animus_model_cli <code.bin> <instruction-count>\n"
                     "Code is loaded at address 0; warp 0 uses all 32 lanes. Count is a host stop condition.\n";
        return 2;
    }
    std::uint64_t count = 0;
    const std::string text = argv[2];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
    ModelConfig config;
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || count == 0 || count > config.max_steps) {
        std::cerr << "Instruction count must be 1..1000000.\n";
        return 2;
    }
    auto created = create_machine(config);
    if (!created) { write_issue(std::cerr, created.error()); return 1; }
    auto& machine = created.value();
    auto loaded = load_binary(machine, {std::filesystem::path(argv[1]), 0, 0});
    if (!loaded) { write_issue(std::cerr, loaded.error()); return 1; }
    auto launched = create_warp(machine, {0, 0xFFFFFFFFu});
    if (!launched) { write_issue(std::cerr, launched.error()); return 1; }
    const auto id = launched.value().value();
    for (std::uint64_t i = 0; i < count; ++i) {
        auto result = step_shader(machine, id);
        if (!result) { write_issue(std::cerr, result.error()); return 1; }
        write_trace(std::cout, result.value());
    }
    std::cerr << "Paused after " << count << " instructions (host limit; not shader completion).\n";
    return 0;
}
