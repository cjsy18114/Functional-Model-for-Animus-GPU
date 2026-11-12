#include "model/model.hpp"
#include <iomanip>
#include <ostream>

namespace animus {
void write_trace(std::ostream& output, const StepRecord& record) {
    // JSON integers preserve exact 32-bit values; no host float formatting is used.
    const auto flags = output.flags();
    output << std::dec << "{\"step\":" << record.step << ",\"warp\":" << record.warp_id
           << ",\"pc_before\":" << record.pc_before << ",\"pc_after\":" << record.pc_after
           << ",\"raw\":" << record.raw << ",\"op\":\"" << op_name(record.op)
           << "\",\"exec_before\":" << record.exec_before << ",\"exec_after\":" << record.exec_after
           << ",\"sp_before\":" << record.sp_before << ",\"sp_after\":" << record.sp_after << ",\"writes\":[";
    bool first = true;
    for (const auto& write : record.register_writes) {
        if (!first) output << ',';
        first = false;
        output << "{\"file\":\"" << (write.file == RegisterFile::Float ? "f" : "x") << "\",\"lane\":" << write.lane << ",\"reg\":" << write.reg
               << ",\"before\":" << write.before << ",\"after\":" << write.after << '}';
    }
    output << "],\"memory_writes\":[";
    first = true;
    for (const auto& write : record.memory_writes) {
        if (!first) output << ',';
        first = false;
        output << "{\"lane\":" << write.lane << ",\"address\":" << write.address
               << ",\"bytes\":" << write.bytes << ",\"before\":" << write.before << ",\"after\":" << write.after << '}';
    }
    output << "],\"csr_writes\":[";
    first = true;
    for (const auto& write : record.csr_writes) {
        if (!first) output << ',';
        first = false;
        output << "{\"lane\":" << write.lane << ",\"csr\":" << write.csr
               << ",\"before_fcsr\":" << write.before << ",\"after_fcsr\":" << write.after << '}';
    }
    output << "],\"experimental_profile\":" << (record.experimental_profile ? "true" : "false")
           << ",\"fp_state_policy\":\"" << (record.fp_state_policy == FpStatePolicy::Unspecified ? "unspecified" : "per-lane-experimental")
           << "\",\"sfu_profile\":\"" << (record.sfu_profile == SfuProfile::Disabled ? "disabled" : "host-math-v1")
           << "\",\"invocation_id\":" << record.invocation_id
           << ",\"ended\":" << (record.status_after == WarpStatus::Ended ? "true" : "false") << ",\"outputs\":";
    if (!record.outputs) output << "null";
    else {
        const auto& batch = *record.outputs;
        output << "{\"stage\":\"" << stage_name(batch.stage) << "\",\"invocation_id\":" << batch.invocation_id << ",\"vertices\":[";
        first = true;
        for (const auto& vertex : batch.vertices) {
            if (!first) output << ',';
            first = false;
            output << "{\"lane\":" << vertex.lane << ",\"vertex_id\":" << vertex.vertex_id << ",\"position_bits\":[";
            for (unsigned i = 0; i < 4; ++i) { if (i) output << ','; output << vertex.position_bits[i]; }
            output << "],\"varying_bits\":[";
            for (unsigned i = 0; i < vertex.varying_count; ++i) { if (i) output << ','; output << vertex.varying_bits[i]; }
            output << "]}";
        }
        output << "],\"fragments\":["; first = true;
        for (const auto& fragment : batch.fragments) {
            if (!first) output << ',';
            first = false;
            const auto& input = fragment.input;
            output << "{\"lane\":" << fragment.lane << ",\"pixel_x\":" << input.pixel_x << ",\"pixel_y\":" << input.pixel_y
                   << ",\"depth_bits\":" << input.depth_bits << ",\"primitive_id\":" << input.primitive_id
                   << ",\"prim_data_addr\":" << input.prim_data_addr << ",\"bary0_bits\":" << input.bary0_bits
                   << ",\"bary1_bits\":" << input.bary1_bits << ",\"rgba8\":" << fragment.rgba8 << '}';
        }
        output << "]}";
    }
    output << ",\"command_offset\":";
    if (record.command_offset) output << *record.command_offset; else output << "null";
    output << ",\"raster_profile\":\"" << (record.raster_profile == RasterProfile::ReferenceV1 ? "reference-v1" : "disabled") << "\",\"warp_events\":[";
    first = true;
    for (const auto& event : record.warp_events) {
        if (!first) output << ',';
        first = false;
        const char* status = event.status == WarpStatus::Ready ? "ready" : event.status == WarpStatus::Waiting ? "waiting" :
                             event.status == WarpStatus::Ended ? "ended" : "faulted";
        const char* wait = event.wait_reason == WaitReason::Barrier ? "barrier" : event.wait_reason == WaitReason::Spawn ? "spawn" : "none";
        output << "{\"warp\":" << event.warp << ",\"invocation_id\":" << event.invocation_id
               << ",\"pc\":" << event.pc << ",\"exec\":" << event.exec
               << ",\"status\":\"" << status << "\",\"wait_reason\":\"" << wait << "\"}";
    }
    output << "]}\n";
    output.flags(flags);
}
void write_issue(std::ostream& output, const Issue& issue) {
    const char* kind = issue.kind == IssueKind::ArchitecturalFault ? "ArchitecturalFault" :
                       issue.kind == IssueKind::ModelLimit ? "ModelLimit" :
                       issue.kind == IssueKind::Diagnostic ? "Diagnostic" : "HostError";
    const auto flags = output.flags();
    output << kind << '/' << issue.code << ": " << issue.message;
    if (issue.warp_id) output << " warp=" << std::dec << *issue.warp_id;
    if (issue.pc) output << " pc=0x" << std::hex << *issue.pc;
    if (issue.raw) output << " raw=0x" << std::hex << *issue.raw;
    if (issue.lane) output << " lane=" << std::dec << *issue.lane;
    if (issue.address) output << " address=0x" << std::hex << *issue.address;
    if (issue.spec_issue_id) output << " spec=" << *issue.spec_issue_id;
    if (issue.command_offset) output << " command_offset=0x" << std::hex << *issue.command_offset;
    output << '\n';
    output.flags(flags);
}
} // namespace animus
