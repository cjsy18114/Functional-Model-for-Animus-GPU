#include "model/model.hpp"
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace {
bool parse_number(std::string_view text, std::uint64_t& value) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) { text.remove_prefix(2); base = 16; }
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
}
int main(int argc, char** argv) {
    using namespace animus;
    const bool stop_mode = argc == 4 && std::string_view(argv[2]) == "--stop-pc";
    if (argc != 3 && !stop_mode) {
        std::cerr << "Usage: animus_model_cli <code.bin> <instruction-count>\n"
                     "   or: animus_model_cli <code.bin> --stop-pc <address>\n"
                     "Code is loaded at address 0; warp 0 uses all 32 lanes. Stops are host pauses.\n";
        return 2;
    }
    std::uint64_t number = 0;
    ModelConfig config;
    if (!parse_number(argv[stop_mode ? 3 : 2], number) ||
        (stop_mode ? number > std::numeric_limits<Addr>::max() || (number & 3u) != 0 : number == 0 || number > config.max_steps)) {
        std::cerr << "Use an aligned 32-bit stop address or an instruction count in 1..1000000.\n";
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
    if (stop_mode) {
        const auto report = run_until_stop(machine, id, static_cast<Addr>(number), &std::cout);
        if (report.issue) { write_issue(std::cerr, *report.issue); return 1; }
        std::cerr << "Paused at stop_pc=" << number << " after " << report.retired_steps << " instructions.\n";
    } else {
        for (std::uint64_t i = 0; i < number; ++i) {
            auto result = step_shader(machine, id);
            if (!result) { write_issue(std::cerr, result.error()); return 1; }
            write_trace(std::cout, result.value());
        }
        std::cerr << "Paused after " << number << " instructions (host limit; not shader completion).\n";
    }
    return 0;
}
