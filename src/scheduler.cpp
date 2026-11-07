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
            if (machine.warps[stop.warp].status == WarpStatus::Ready && machine.warps[stop.warp].pc == stop.stop_pc)
                paused |= Mask{1} << stop.warp;
        if (paused == selected) return {StopReason::Paused, machine.retired_steps - before, {}};
        auto next = choose_ready(machine, last, paused | (~selected & 15u));
        if (!next) return {StopReason::SuspectedDeadlock, machine.retired_steps - before,
            make_issue(IssueKind::ModelLimit, "NoReadyWarp", "Selected unfinished warps are waiting; no hardware barrier semantics are inferred.")};
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
