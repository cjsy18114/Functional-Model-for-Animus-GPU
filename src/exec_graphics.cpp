#include "model/execution.hpp"
#include <cstdint>

namespace animus {
namespace {
bool graphics_stage(ShaderStage stage) {
    return stage == ShaderStage::Vertex || stage == ShaderStage::Fragment;
}
Issue graphics_fault(const char* code, const char* message) {
    return make_issue(IssueKind::ArchitecturalFault, code, message);
}
Result<void> validate_texture(const Memory& memory, const GraphicsState& state) {
    if (state.texture_width == 0 || state.texture_height == 0)
        return graphics_fault("EmptyTexture", "gfx.tex requires nonzero texture dimensions.");
    const auto row_bytes = std::uint64_t{state.texture_width} * 4;
    if ((state.texture_base & 3u) || (state.texture_stride & 3u) || state.texture_stride < row_bytes)
        return graphics_fault("InvalidTextureLayout", "RGBA8 base/stride must be word aligned and stride must contain a row.");
    // Wide products and an end-exclusive range prevent 32-bit address wrap.
    const auto bytes = std::uint64_t{state.texture_height - 1} * state.texture_stride + row_bytes;
    if (std::uint64_t{state.texture_base} + bytes > (std::uint64_t{1} << 32))
        return graphics_fault("TextureAddressOverflow", "Texture extent exceeds the 32-bit address space.");
    return memory.check_access(state.texture_base, bytes, Access::Read);
}
unsigned texel_coordinate(Word bits, Word extent) {
    const Word magnitude = bits & 0x7FFFFFFFu;
    // All NaNs, all negatives (including -Inf), and either zero clamp to zero.
    if (magnitude > 0x7F800000u || (bits >> 31) || magnitude == 0) return 0;
    if (magnitude >= 0x3F800000u) return extent - 1; // +Inf and finite u >= 1.
    const unsigned exponent = magnitude >> 23;
    const Word significand = (magnitude & 0x7FFFFFu) | (exponent ? 0x800000u : 0);
    const unsigned shift = exponent ? 150 - exponent : 149;
    // Exact floor of the FP32 input times an integer extent. No intermediate
    // host FP rounding, guest frm, host libm, or SFU approximation is involved.
    const auto product = std::uint64_t{significand} * extent;
    return shift >= 64 ? 0 : static_cast<unsigned>(product >> shift);
}
}
const char* stage_name(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Test: return "test";
    case ShaderStage::Vertex: return "vs";
    case ShaderStage::Fragment: return "fs";
    }
    return "invalid";
}
Result<void> validate_dispatch(const Machine& machine, const LaunchInfo& launch) {
    const auto& context = launch.dispatch;
    if (context.stage != ShaderStage::Test && !graphics_stage(context.stage))
        return make_issue(IssueKind::HostError, "InvalidShaderStage", "Host dispatch stage must be Test, Vertex or Fragment.");
    if (context.varying_count > 8)
        return make_issue(IssueKind::HostError, "InvalidVaryingCount", "At most eight varying components are supported.");
    if (context.stage == ShaderStage::Test) return {};
    const auto& texture = context.graphics;
    if ((texture.texture_width == 0) != (texture.texture_height == 0))
        return make_issue(IssueKind::HostError, "InvalidTextureDimensions", "Texture width/height must both be zero or positive.");
    if (texture.texture_width > machine.config.max_width || texture.texture_height > machine.config.max_height)
        return make_issue(IssueKind::ModelLimit, "TextureDimensionLimit", "Texture exceeds configured host resource limits.");
    if (texture.texture_width) {
        auto checked = validate_texture(machine.memory, texture);
        if (!checked) return checked.error();
    }
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if (!(launch.init_mask & (Mask{1} << lane))) continue;
        const auto& input = context.inputs[lane];
        // These are minimum extents, not inferred allocation lengths. The host
        // supplies exact mapped regions; guest LW/FLW still check each access.
        auto uniform = machine.memory.check_access(input.uniform_base, 1, Access::Read);
        if (!uniform) return uniform.error();
        const Addr address = context.stage == ShaderStage::Vertex ? input.vertex_addr : input.prim_data_addr;
        if (address & 3u) return make_issue(IssueKind::HostError, "InvalidInputAddress", "Vertex/interpolation input must be word aligned.");
        const std::uint64_t bytes = context.stage == ShaderStage::Vertex ? 1 : 12 + 12 * context.varying_count;
        auto checked = machine.memory.check_access(address, bytes, Access::Read);
        if (!checked) return checked.error();
    }
    return {};
}
Result<Word> get_input(const DispatchContext& context, unsigned lane, std::uint16_t selector) {
    if (lane >= kMaxLanes) return make_issue(IssueKind::HostError, "InvalidInputLane", "Input lane is outside the context.");
    if (!graphics_stage(context.stage)) return graphics_fault("WrongStage", "Graphics input requires VS or FS dispatch.");
    const auto& input = context.inputs[lane];
    if (selector == 0x002) return input.uniform_base;
    if (selector <= 0x001) {
        if (context.stage != ShaderStage::Vertex) return graphics_fault("WrongStage", "Vertex selector used outside VS.");
        return selector == 0 ? input.vertex_id : input.vertex_addr;
    }
    if (selector >= 0x010 && selector <= 0x016) {
        if (context.stage != ShaderStage::Fragment) return graphics_fault("WrongStage", "Fragment selector used outside FS.");
        switch (selector) {
        case 0x010: return input.pixel_x;
        case 0x011: return input.pixel_y;
        case 0x012: return input.depth_bits;
        case 0x013: return input.primitive_id;
        case 0x014: return input.prim_data_addr;
        case 0x015: return input.bary0_bits;
        case 0x016: return input.bary1_bits;
        }
    }
    return graphics_fault("InvalidSelector", "gfx.get selector is not defined by the ISA.");
}
Result<Word> sample_texture(const Memory& memory, const GraphicsState& state, Word u_bits, Word v_bits) {
    auto checked = validate_texture(memory, state);
    if (!checked) return checked.error();
    const auto x = texel_coordinate(u_bits, state.texture_width);
    const auto y = texel_coordinate(v_bits, state.texture_height);
    const auto address = std::uint64_t{state.texture_base} + std::uint64_t{y} * state.texture_stride + std::uint64_t{x} * 4;
    return memory.load_le(static_cast<Addr>(address), 4); // Layout/whole extent already validated.
}
Result<OutputBatch> collect_outputs(const WarpState& warp, bool check_undefined) {
    if (!graphics_stage(warp.dispatch.stage)) return graphics_fault("WrongStage", "gfx.end requires VS or FS dispatch.");
    if (warp.sp != 0 || warp.exec != warp.init_mask || warp.init_mask == 0)
        return graphics_fault("UnreconvergedEnd", "gfx.end requires SP=0 and EXEC=INIT_MASK.");
    if (warp.dispatch.varying_count > 8)
        return make_issue(IssueKind::HostError, "InvalidVaryingCount", "Invalid dispatch varying count.");
    OutputBatch batch; batch.stage = warp.dispatch.stage; batch.invocation_id = warp.invocation_id;
    for (unsigned lane = 0; lane < kMaxLanes; ++lane) {
        if (!(warp.init_mask & (Mask{1} << lane))) continue;
        const auto& registers = warp.lanes[lane];
        if (batch.stage == ShaderStage::Vertex) {
            VertexOutput output; output.invocation_id = warp.invocation_id; output.lane = lane;
            output.vertex_id = warp.dispatch.inputs[lane].vertex_id;
            output.varying_count = warp.dispatch.varying_count;
            for (unsigned reg = 0; reg < 4 + output.varying_count; ++reg) {
                if (check_undefined && !registers.f_defined[reg]) {
                    auto issue = make_issue(IssueKind::Diagnostic, "UndefinedGraphicsOutput", "VS implicit output register is uninitialized.");
                    issue.lane = lane; return issue;
                }
                if (reg < 4) output.position_bits[reg] = registers.f_bits[reg];
                else output.varying_bits[reg - 4] = registers.f_bits[reg];
            }
            batch.vertices.push_back(output);
        } else {
            if (check_undefined && !registers.x_defined[10]) {
                auto issue = make_issue(IssueKind::Diagnostic, "UndefinedGraphicsOutput", "FS implicit x10 output is uninitialized.");
                issue.lane = lane; return issue;
            }
            batch.fragments.push_back({warp.invocation_id, lane, warp.dispatch.inputs[lane], registers.x[10]});
        }
    }
    return batch;
}
Result<StepEffect> eval_graphics(const Machine& machine, WarpId id, const DecodedInst& inst) {
    if (inst.op != Op::GraphicsGet && inst.op != Op::GraphicsTexture && inst.op != Op::GraphicsEnd)
        return make_issue(IssueKind::HostError, "InvalidDecodedOp", "Not a graphics operation.");
    const auto& warp = machine.warps[id];
    if (!graphics_stage(warp.dispatch.stage)) return graphics_fault("WrongStage", "Graphics instructions require an explicit VS or FS context.");
    StepEffect effect; effect.next_pc = warp.pc + Word{4};
    if (inst.op == Op::GraphicsEnd) {
        auto outputs = collect_outputs(warp, machine.config.check_undefined);
        if (!outputs) return outputs.error();
        effect.outputs = std::move(outputs.value()); effect.next_status = WarpStatus::Ended;
        return effect; // Commit owns a complete copy before the warp can be released.
    }
    for (unsigned lane = 0; lane < machine.config.warp_width; ++lane) {
        if (!(warp.exec & (Mask{1} << lane))) continue;
        auto value = inst.op == Op::GraphicsGet ? get_input(warp.dispatch, lane, inst.selector) :
            sample_texture(machine.memory, warp.dispatch.graphics, read_x(warp, lane, inst.rs1), read_x(warp, lane, inst.rs2));
        if (!value) { auto issue = value.error(); issue.lane = lane; return issue; }
        append_x_write(effect, warp, lane, inst.rd, value.value()); // x0 discards data, not legality checks.
    }
    return effect;
}
} // namespace animus
