#include "model/model.hpp"
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
    std::vector<std::string_view> arguments;
    bool numeric = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--experimental-numeric") numeric = true;
        else arguments.emplace_back(argv[index]);
    }
    const bool stop_mode = arguments.size() == 3 && arguments[1] == "--stop-pc";
    const bool management_mode = arguments.size() == 2 && arguments[1] == "--run";
    if (arguments.size() != 2 && !stop_mode) {
        std::cerr << "Usage: animus_model_cli <code.bin> <instruction-count>\n"
                     "   or: animus_model_cli <code.bin> --stop-pc <address>\n"
                     "   or: animus_model_cli <code.bin> --run (all management warps until TMC zero)\n"
                     "Optional: --experimental-numeric (S04 per-lane fcsr and S05 HostMathV1).\nCode is loaded at address 0; warp 0 uses all 32 lanes. Stops are host pauses.\n";
        return 2;
    }
    std::uint64_t number = 0;
    ModelConfig config;
    if (numeric) {
        config.fp_state_policy = FpStatePolicy::PerLaneExperimental;
        config.sfu_profile = SfuProfile::HostMathV1;
        std::cerr << "Experimental numeric profiles: per-lane fcsr; HostMathV1 SFU. Not approved hardware semantics.\n";
    }
    if (!management_mode && (!parse_number(arguments[stop_mode ? 2 : 1], number) ||
        (stop_mode ? number > std::numeric_limits<Addr>::max() || (number & 3u) != 0 : number == 0 || number > config.max_steps))) {
        std::cerr << "Use an aligned 32-bit stop address or an instruction count in 1..1000000.\n";
        return 2;
    }
    auto created = create_machine(config);
    if (!created) { write_issue(std::cerr, created.error()); return 1; }
    auto& machine = created.value();
    auto loaded = load_binary(machine, {std::filesystem::path(std::string(arguments[0])), 0, 0});
    if (!loaded) { write_issue(std::cerr, loaded.error()); return 1; }
    auto launched = create_warp(machine, {0, 0xFFFFFFFFu});
    if (!launched) { write_issue(std::cerr, launched.error()); return 1; }
    const auto id = launched.value().value();
    if (management_mode) {
        const auto report = run_management(machine, &std::cout);
        if (report.issue) { write_issue(std::cerr, *report.issue); return 1; }
        std::cerr << "Completed management program after " << report.retired_steps << " instructions.\n";
    } else if (stop_mode) {
        const auto report = run_until_stop(machine, id, static_cast<Addr>(number), &std::cout);
        if (report.issue) { write_issue(std::cerr, *report.issue); return 1; }
        std::cerr << "Paused at stop_pc=" << number << " after " << report.retired_steps << " instructions.\n";
    } else {
        for (std::uint64_t i = 0; i < number; ++i) {
            auto result = step_shader(machine, id);
            if (!result) { write_issue(std::cerr, result.error()); return 1; }
            write_trace(std::cout, result.value());
            if (machine.warps[id].status != WarpStatus::Ready) {
                std::cerr << "Warp ended or is waiting; use --run to include spawned/peer warps.\n";
                return 0;
            }
        }
        std::cerr << "Paused after " << number << " instructions (host limit; not shader completion).\n";
    }
    return 0;
}
