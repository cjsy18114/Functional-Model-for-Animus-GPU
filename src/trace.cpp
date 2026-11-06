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
        output << "{\"lane\":" << write.lane << ",\"reg\":" << write.reg
               << ",\"before\":" << write.before << ",\"after\":" << write.after << '}';
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
    output << '\n';
    output.flags(flags);
}
} // namespace animus
