#include "model/submission.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>

namespace animus {
namespace {
Issue fault(const char* code, const char* message) { return make_issue(IssueKind::ArchitecturalFault,code,message); }
bool overlap(Addr a, std::uint64_t an, Addr b, std::uint64_t bn) {
    return std::uint64_t{a} < std::uint64_t{b}+bn && std::uint64_t{b} < std::uint64_t{a}+an;
}
Result<std::uint64_t> image_extent(Addr base, Word width, Word height, Word stride) {
    if (!width || !height || (base & 3u) || (stride & 3u) || std::uint64_t{stride} < std::uint64_t{width}*4)
        return fault("InvalidImageLayout","Image dimensions, base or stride are invalid.");
    const auto size = std::uint64_t{height-1}*stride + std::uint64_t{width}*4;
    if (size > (std::uint64_t{1}<<32)-base) return fault("ImageAddressOverflow","Image exceeds the 32-bit address space.");
    return size;
}
Result<void> target(const Machine& machine, const GraphicsState& state, bool depth, bool read) {
    const auto base = depth ? state.depth_base : state.color_base;
    auto size = image_extent(base,state.width,state.height,depth ? state.depth_stride : state.color_stride);
    if (!size) return size.error();
    const auto role = depth ? ResourceRole::Depth : ResourceRole::Color;
    auto checked = require_resource(machine,role,base,size.value(),read ? Access::Read : Access::Write);
    if (!checked) return checked.error();
    for (const auto& resource : machine.resources) {
        // Other attachments can be rebound across draws. Compare the currently
        // bound color/depth pair separately, but protect all input/temporary data.
        if (resource.role == ResourceRole::Color || resource.role == ResourceRole::Depth) continue;
        if (overlap(base,size.value(),resource.base,resource.bytes)) return fault("AttachmentOverlap","Attachment overlaps input, command, scratch or completion storage.");
    }
    if (depth) {
        auto color_size = image_extent(state.color_base,state.width,state.height,state.color_stride);
        if (!color_size) return color_size.error();
        if (overlap(base,size.value(),state.color_base,color_size.value())) return fault("AttachmentOverlap","Color and depth attachments overlap.");
    }
    return {};
}
void fail_submission(Machine& machine, Issue issue, Word code, Word offset) {
    auto& submission = machine.submission;
    issue.command_offset = offset;
    submission.status = SubmissionStatus::Error; submission.error_code = code; submission.error_offset = offset;
    submission.last_issue = std::move(issue);
    // No in-flight transactions exist in this synchronous model. Release only
    // the owned faulted shader; preserve all earlier memory side effects.
    if (submission.draw && submission.draw->warp) {
        const auto id = *submission.draw->warp;
        if (machine.warps[id].allocated && machine.warps[id].status == WarpStatus::Faulted) (void)release_warp(machine,id);
    }
}
Word error_category(const Issue& issue, Word fallback) {
    if (issue.warp_id) return 5;
    if (issue.code == "InvalidDrawCount") return 2;
    if (issue.code == "VertexAddressOverflow") return 4;
    if (issue.address || issue.code == "MemoryRange" || issue.code == "MemoryPermission" ||
        issue.code == "ResourceRange" || issue.code == "ImageAddressOverflow") return 4;
    return fallback;
}
void retire_command(SubmissionState& submission) {
    ++submission.completed_commands; submission.offset += 16;
    submission.current.reset(); submission.operation_started = false; submission.clear_index = 0; submission.draw.reset();
}
Result<bool> advance_clear(Machine& machine, const Command& command) {
    auto& submission = machine.submission; const auto& state = submission.graphics;
    if (!submission.operation_started) {
        if (command.a & 1u) { auto checked = target(machine,state,false,false); if (!checked) return checked.error(); }
        if (command.a & 2u) {
            const Word magnitude = command.c & 0x7FFFFFFFu;
            if (magnitude > 0x3F800000u || ((command.c >> 31) && magnitude))
                return fault("InvalidClearDepth","Selected clear depth must be finite in [0,1].");
            auto checked = target(machine,state,true,false); if (!checked) return checked.error();
        }
        submission.operation_started = true;
    }
    if (command.a == 0) return true; // Unselected B/C are intentionally ignored.
    if (submission.clear_index == std::uint64_t{state.width}*state.height) return true;
    auto work = consume_work(machine); if (!work) return work.error();
    const auto y = submission.clear_index/state.width, x = submission.clear_index%state.width;
    if (command.a & 1u) machine.memory.store_le_prechecked(static_cast<Addr>(state.color_base+y*state.color_stride+x*4),4,command.b);
    if (command.a & 2u) machine.memory.store_le_prechecked(static_cast<Addr>(state.depth_base+y*state.depth_stride+x*4),4,command.c);
    ++submission.clear_index;
    return false;
}
Result<void> write_completion(Machine& machine, const Command& command) {
    if (command.a & 3u) return fault("CompletionAlignment","Completion address must be word aligned.");
    auto resource = require_resource(machine,ResourceRole::Completion,command.a,4,Access::Write);
    if (!resource) return resource.error();
    for (const auto& other : machine.resources)
        if (other.role != ResourceRole::Completion && overlap(command.a,4,other.base,other.bytes))
            return fault("CompletionOverlap","Completion word overlaps submission resources.");
    return machine.memory.store_le(command.a,4,command.b);
}
}
Result<void> consume_work(Machine& machine) {
    if (machine.submission.work_items >= machine.config.max_work_items)
        return make_issue(IssueKind::ModelLimit,"WorkBudget","Submission work-item budget exhausted; raise the host limit to resume.");
    ++machine.submission.work_items; return {};
}
Result<Word> mmio_read(const Machine& machine, Word offset) {
    const auto& s = machine.submission;
    switch (offset) {
    case 0: return s.cmd_base; case 4: return s.cmd_bytes;
    case 12: return static_cast<Word>(s.status); case 16: return s.error_code; case 20: return s.error_offset;
    default: return make_issue(IssueKind::HostError,"InvalidMmioRead","Unknown or write-only MMIO offset.");
    }
}
Result<void> mmio_write(Machine& machine, Word offset, Word value) {
    auto& s = machine.submission;
    if (offset != 0 && offset != 4 && offset != 8) return make_issue(IssueKind::HostError,"InvalidMmioWrite","Unknown or read-only MMIO offset.");
    if (s.status == SubmissionStatus::Busy) return {};
    if (offset == 0) { s.cmd_base = value; return {}; }
    if (offset == 4) { s.cmd_bytes = value; return {}; }
    if (value != 1) return {};
    const Word base = s.cmd_base, bytes = s.cmd_bytes;
    s = SubmissionState{}; s.cmd_base = s.latched_base = base; s.cmd_bytes = s.latched_bytes = bytes; s.status = SubmissionStatus::Busy;
    if ((base & 15u) || bytes < 16 || (bytes & 15u) || std::uint64_t{base}+bytes > (std::uint64_t{1}<<32)) {
        fail_submission(machine,fault("InvalidSubmission","Command range must be aligned, nonempty and nonwrapping."),1,0xFFFFFFFFu); return {};
    }
    auto resource = require_resource(machine,ResourceRole::Command,base,bytes,Access::Read);
    if (!resource) fail_submission(machine,resource.error(),1,0xFFFFFFFFu);
    return {};
}
Result<Command> fetch_command(const Machine& machine, Addr base, Word bytes, Word offset) {
    if ((base & 15u) || (offset & 15u) || offset > bytes || bytes-offset < 16 || std::uint64_t{base}+offset+16 > (std::uint64_t{1}<<32))
        return fault("MissingEnd","No complete command/END remains inside the latched buffer.");
    auto resource = require_resource(machine,ResourceRole::Command,base+offset,16,Access::Read);
    if (!resource) return resource.error();
    const auto address = base+offset;
    Command command{machine.memory.peek_le_prechecked(address,4),machine.memory.peek_le_prechecked(address+4,4),
                    machine.memory.peek_le_prechecked(address+8,4),machine.memory.peek_le_prechecked(address+12,4)};
    bool valid = !(command.opcode & 0xFFFFFF00u);
    switch (command.opcode) {
    case 0: case 255: valid &= command.a == 0 && command.b == 0 && command.c == 0; break;
    case 1: valid &= command.b == 0 && command.c == 0; break;
    case 2: valid &= (command.a & ~3u) == 0; break;
    case 3: case 4: valid &= command.c == 0; break;
    default: valid = false;
    }
    if (!valid) return fault("InvalidCommand","Unknown opcode, reserved field or unused parameter is nonzero.");
    if (command.opcode == 255 && bytes-offset != 16) return fault("EarlyEnd","END must be the last command.");
    return command;
}
Result<GraphicsState> read_state(const Machine& machine, Addr descriptor) {
    if (descriptor & 63u) return fault("StateAlignment","State descriptor must be 64-byte aligned.");
    auto resource = require_resource(machine,ResourceRole::Descriptor,descriptor,128,Access::Read);
    if (!resource) return resource.error();
    std::array<Word,32> words{};
    for (unsigned i = 0; i < 32; ++i) words[i] = machine.memory.peek_le_prechecked(descriptor+i*4,4);
    for (unsigned i = 26; i < 32; ++i) if (words[i]) return fault("StateReserved","State reserved words must be zero.");
    GraphicsState s;
    s.version=words[0]; s.flags=words[1]; s.color_base=words[2]; s.depth_base=words[3];
    s.width=words[4]; s.height=words[5]; s.color_stride=words[6]; s.depth_stride=words[7];
    s.vs_entry=words[8]; s.fs_entry=words[9]; s.vertex_base=words[10]; s.vertex_stride=words[11];
    s.uniform_base=words[12]; s.varying_count=words[13]; s.texture_base=words[14];
    s.texture_width=words[15]; s.texture_height=words[16]; s.texture_stride=words[17];
    s.viewport_x=std::bit_cast<std::int32_t>(words[18]); s.viewport_y=std::bit_cast<std::int32_t>(words[19]);
    s.viewport_width=words[20]; s.viewport_height=words[21];
    s.scissor_x=std::bit_cast<std::int32_t>(words[22]); s.scissor_y=std::bit_cast<std::int32_t>(words[23]);
    s.scissor_width=words[24]; s.scissor_height=words[25]; return s;
}
Result<void> validate_state(const Machine& machine, const GraphicsState& s) {
    if (s.version != 1 || (s.flags & ~7u) || s.varying_count > 8 || !s.width || !s.height || !s.viewport_width || !s.viewport_height)
        return fault("InvalidState","Invalid descriptor version, flags, dimensions or varying count.");
    if ((s.color_stride & 3u) || (s.depth_stride & 3u) || (s.texture_stride & 3u))
        return fault("StateStrideAlignment","Color/depth/texture strides must be multiples of four, including unused fields.");
    for (const Addr address : {s.color_base,s.depth_base,s.vs_entry,s.fs_entry,s.vertex_base,s.uniform_base,s.texture_base})
        if (address & 3u) return fault("StateAddressAlignment","Descriptor address fields must be word aligned.");
    if (s.width > machine.config.max_width || s.height > machine.config.max_height || s.viewport_width > machine.config.max_width ||
        s.viewport_height > machine.config.max_height || s.texture_width > machine.config.max_width || s.texture_height > machine.config.max_height)
        return make_issue(IssueKind::ModelLimit,"ImageDimensionLimit","Descriptor exceeds the current model's declared host capacity.");
    auto color = target(machine,s,false,false); if (!color) return color.error();
    if (s.flags & 3u) {
        auto depth = target(machine,s,true,(s.flags & 1u) != 0); if (!depth) return depth.error();
        if (s.flags & 2u) { auto write = target(machine,s,true,false); if (!write) return write.error(); }
    }
    if ((s.texture_width == 0) != (s.texture_height == 0)) return fault("TextureDimensions","Texture dimensions must both be zero or positive.");
    if (s.texture_width) {
        auto size = image_extent(s.texture_base,s.texture_width,s.texture_height,s.texture_stride); if (!size) return size.error();
        auto texture = require_resource(machine,ResourceRole::Texture,s.texture_base,size.value(),Access::Read); if (!texture) return texture.error();
    }
    return {};
}
RunReport run_submission(Machine& machine, std::ostream* trace) {
    auto& s = machine.submission; const auto retired_before = machine.retired_steps;
    if (s.status == SubmissionStatus::Idle) return {StopReason::HostError,0,make_issue(IssueKind::HostError,"NoSubmission","Ring the existing doorbell first.")};
    while (s.status == SubmissionStatus::Busy) {
        Result<void> operation;
        Word category = 2;
        if (!s.current) {
            operation = consume_work(machine);
            if (operation) {
                auto command = fetch_command(machine,s.latched_base,s.latched_bytes,s.offset);
                if (!command) operation = command.error();
                else s.current = command.value();
            }
        }
        if (operation && s.current) {
            const auto command = *s.current;
            bool complete = true;
            if (command.opcode == 1) {
                category = 3; s.state_valid = false;
                auto state = read_state(machine,command.a);
                if (!state) operation = state.error();
                else { operation = validate_state(machine,state.value()); if (operation) { s.graphics=state.value(); s.state_valid=true; } }
            } else if (command.opcode == 2 || command.opcode == 3) {
                category = command.opcode == 2 ? 3 : 5;
                if (!s.state_valid) operation = fault("StateNotBound","CLEAR/DRAW requires a successful BIND_STATE.");
                else if (command.opcode == 2) { auto result = advance_clear(machine,command); if (!result) operation=result.error(); else complete=result.value(); }
                else { auto result = advance_draw(machine,trace); if (!result) operation=result.error(); else complete=result.value(); }
                if (!s.state_valid) category = 3;
            } else if (command.opcode == 4) { category = 4; operation = write_completion(machine,command); }
            if (operation && complete) {
                retire_command(s);
                if (command.opcode == 255) s.status=SubmissionStatus::Done;
            }
        }
        if (!operation) {
            auto issue=operation.error(); issue.command_offset=s.offset;
            if (issue.kind == IssueKind::ArchitecturalFault) { fail_submission(machine,issue,error_category(issue,category),s.offset); break; }
            s.last_issue=issue;
            return {issue.code=="WorkBudget" || issue.code=="StepBudget" ? StopReason::Timeout :
                issue.kind==IssueKind::ModelLimit ? StopReason::ModelLimit : issue.kind==IssueKind::Diagnostic ? StopReason::Diagnostic : StopReason::HostError,
                machine.retired_steps-retired_before,issue};
        }
    }
    return {s.status==SubmissionStatus::Done ? StopReason::Completed : StopReason::ArchitecturalFault,
            machine.retired_steps-retired_before,s.status==SubmissionStatus::Error ? s.last_issue : std::optional<Issue>{}};
}
} // namespace animus
