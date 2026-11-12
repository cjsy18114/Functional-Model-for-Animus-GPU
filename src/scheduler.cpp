#include "model/model.hpp"
#include <optional>
#include <span>

namespace animus {
RunReport run_warps_until_stops(Machine& machine, std::span<const WarpStop> stops, std::ostream* trace) {
    if (stops.empty() || stops.size() > kWarpSlots)
        return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "InvalidWarpBatch", "Select one to four unique live warps.")};
    Mask selected = 0;
    for (const auto& stop : stops) {
        if (stop.warp >= kWarpSlots || !machine.warps[stop.warp].allocated || (stop.stop_pc & 3u) != 0 ||
            (selected & (Mask{1} << stop.warp)) != 0)
            return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "InvalidWarpBatch", "Invalid warp, duplicate warp or unaligned host stop PC.")};
        if (machine.warps[stop.warp].status == WarpStatus::Ended || machine.warps[stop.warp].status == WarpStatus::Faulted)
            return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "WarpNotLive", "Batch cannot resume an ended/faulted warp.")};
        selected |= Mask{1} << stop.warp;
    }
    const auto before = machine.retired_steps;
    std::optional<WarpId> last;
    while (true) {
        Mask paused = 0;
        for (const auto& stop : stops)
            if (machine.warps[stop.warp].status == WarpStatus::Ended ||
                (machine.warps[stop.warp].status == WarpStatus::Ready && machine.warps[stop.warp].pc == stop.stop_pc))
                paused |= Mask{1} << stop.warp;
        if (paused == selected) return {StopReason::Paused, machine.retired_steps - before, {}};
        auto next = choose_ready(machine, last, paused | (~selected & 15u));
        if (!next) return {StopReason::SuspectedDeadlock, machine.retired_steps - before,
            make_issue(IssueKind::ModelLimit, "NoReadyWarp", "Selected unfinished warps are waiting; required peers may be excluded or missing.")};
        auto result = step_shader(machine, *next);
        if (!result) {
            const auto& issue = result.error();
            const auto reason = issue.code == "StepBudget" ? StopReason::Timeout :
                issue.kind == IssueKind::ArchitecturalFault ? StopReason::ArchitecturalFault :
                issue.kind == IssueKind::ModelLimit ? StopReason::ModelLimit :
                issue.kind == IssueKind::Diagnostic ? StopReason::Diagnostic : StopReason::HostError;
            return {reason, machine.retired_steps - before, issue};
        }
        if (trace) write_trace(*trace, result.value());
        last = next;
    }
}
RunReport run_shader_batch(Machine& machine, std::span<const WarpId> ids, std::ostream* trace) {
    Mask selected = 0;
    if (ids.empty() || ids.size() > kWarpSlots)
        return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "InvalidShaderBatch", "Select one to four unique VS/FS warps.")};
    for (const auto id : ids) {
        if (id >= kWarpSlots || !machine.warps[id].allocated || (selected & (Mask{1} << id)) ||
            machine.warps[id].status == WarpStatus::Faulted || machine.warps[id].dispatch.stage == ShaderStage::Test)
            return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "InvalidShaderBatch", "Invalid, duplicate, faulted or non-graphics warp.")};
        selected |= Mask{1} << id;
    }
    const auto before = machine.retired_steps;
    std::optional<WarpId> last;
    while (true) {
        Mask ended = 0;
        for (const auto id : ids) if (machine.warps[id].status == WarpStatus::Ended) ended |= Mask{1} << id;
        if (ended == selected) return {StopReason::Completed, machine.retired_steps - before, {}};
        const auto next = choose_ready(machine, last, ~selected & 15u);
        if (!next) return {StopReason::SuspectedDeadlock, machine.retired_steps - before,
            make_issue(IssueKind::ModelLimit, "NoReadyWarp", "No selected unfinished shader is ready.")};
        auto result = step_shader(machine, *next);
        if (!result) {
            const auto& issue = result.error();
            const auto reason = issue.code == "StepBudget" ? StopReason::Timeout :
                issue.kind == IssueKind::ArchitecturalFault ? StopReason::ArchitecturalFault :
                issue.kind == IssueKind::ModelLimit ? StopReason::ModelLimit :
                issue.kind == IssueKind::Diagnostic ? StopReason::Diagnostic : StopReason::HostError;
            return {reason, machine.retired_steps - before, issue};
        }
        if (trace) write_trace(*trace, result.value());
        last = next;
    }
}
RunReport run_management(Machine& machine, std::ostream* trace) {
    const auto before = machine.retired_steps;
    if (machine.submission.status == SubmissionStatus::Busy)
        return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "SubmissionInProgress", "Use the submission runner for graphics work.")};
    for (const auto& warp : machine.warps)
        if (warp.allocated && warp.dispatch.stage != ShaderStage::Test)
            return {StopReason::HostError, 0, make_issue(IssueKind::HostError, "NotManagementContext", "Collect/release VS/FS contexts before management execution.")};
    std::optional<WarpId> last;
    while (true) {
        bool unfinished = false;
        for (const auto& warp : machine.warps) {
            if (!warp.allocated) continue;
            if (warp.status == WarpStatus::Faulted) {
                auto issue = make_issue(IssueKind::ArchitecturalFault, "FaultedWarp", "A management warp is already faulted.");
                issue.warp_id = warp.id; issue.pc = warp.pc;
                return {StopReason::ArchitecturalFault, machine.retired_steps - before, issue};
            }
            if (warp.status != WarpStatus::Ended) unfinished = true;
        }
        if (!unfinished && !machine.pending_spawn)
            return {StopReason::Completed, machine.retired_steps - before, {}};
        const auto next = choose_ready(machine, last);
        if (!next)
            return {StopReason::SuspectedDeadlock, machine.retired_steps - before,
                make_issue(IssueKind::ModelLimit, "NoReadyWarp", "All unfinished warps are waiting; check barrier counts/IDs and deferred spawn dependencies.")};
        auto result = step_shader(machine, *next);
        if (!result) {
            const auto& issue = result.error();
            const auto reason = issue.code == "StepBudget" ? StopReason::Timeout :
                issue.kind == IssueKind::ArchitecturalFault ? StopReason::ArchitecturalFault :
                issue.kind == IssueKind::ModelLimit ? StopReason::ModelLimit :
                issue.kind == IssueKind::Diagnostic ? StopReason::Diagnostic : StopReason::HostError;
            return {reason, machine.retired_steps - before, issue};
        }
        if (trace) write_trace(*trace, result.value());
        last = next;
    }
}
} // namespace animus
