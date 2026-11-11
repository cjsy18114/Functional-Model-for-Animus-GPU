#pragma once
#include "model/model.hpp"
#include <optional>
#include <span>
#include <vector>

namespace animus {
// All addresses are simulated GPU addresses. Resource metadata is host-only.
Result<Resource> require_resource(const Machine&, ResourceRole, Addr, std::uint64_t, Access);
Result<Word> mmio_read(const Machine&, Word offset);
Result<void> mmio_write(Machine&, Word offset, Word value);
Result<Command> fetch_command(const Machine&, Addr base, Word bytes, Word offset);
Result<GraphicsState> read_state(const Machine&, Addr descriptor);
Result<void> validate_state(const Machine&, const GraphicsState&);
RunReport run_submission(Machine&, std::ostream* shader_trace = nullptr);
// Internal resumable primitives: each successful work debit happens before its
// side effect, and progress advances immediately afterwards (no replay on resume).
Result<void> consume_work(Machine&);
Result<bool> advance_draw(Machine&, std::ostream*);
Result<std::vector<Triangle>> clip_triangle(const std::array<VertexOutput,3>&, Word primitive_id, unsigned varying_count);
Result<std::optional<RasterTriangle>> setup_triangle(const Triangle&, const GraphicsState&);
Result<Addr> write_interp_record(Machine&, const RasterTriangle&, unsigned varying_count);
Result<std::optional<InvocationInput>> raster_sample(const RasterTriangle&, const GraphicsState&, Addr interpolation, std::uint64_t index);
Result<void> merge_fragment(Machine&, const FragmentOutput&);
Word reference_f32(double); // RNE quantization without modifying guest fcsr.
} // namespace animus
