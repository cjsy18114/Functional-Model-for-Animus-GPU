#include "model/submission.hpp"
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace animus {
namespace {
bool contains(const Resource& resource, Addr base, std::uint64_t bytes) {
    return base >= resource.base && std::uint64_t{base} - resource.base <= resource.bytes &&
           bytes <= resource.bytes - (std::uint64_t{base} - resource.base);
}
bool overlaps(const Resource& a, const Resource& b) {
    return a.base < std::uint64_t{b.base}+b.bytes && b.base < std::uint64_t{a.base}+a.bytes;
}
}
Result<void> install_resources(Machine& machine, std::span<const Resource> resources) {
    if (machine.submission.status == SubmissionStatus::Busy)
        return make_issue(IssueKind::HostError,"ResourcesInUse","Do not replace resource metadata while a submission is busy or paused.");
    for (const auto& resource : resources) {
        if (static_cast<unsigned>(resource.role) > static_cast<unsigned>(ResourceRole::Completion) || resource.bytes == 0 ||
            resource.bytes > (std::uint64_t{1} << 32)-resource.base || resource.permissions == 0 || (resource.permissions & ~7u))
            return make_issue(IssueKind::HostError,"InvalidResource","Invalid role, extent or permissions in host resource metadata.");
        const bool input = resource.role <= ResourceRole::Texture;
        if (input && (resource.permissions & permission(Access::Write)))
            return make_issue(IssueKind::HostError,"WritableInputResource","Commands, descriptors, shaders and input resources must be read-only to guest execution.");
        for (const auto access : {Access::Read,Access::Write,Access::Execute}) {
            if (!(resource.permissions & permission(access))) continue;
            auto checked = machine.memory.check_access(resource.base,resource.bytes,access);
            if (!checked) { auto issue=checked.error(); issue.kind=IssueKind::HostError; issue.code="InvalidResourceMapping"; return issue; }
        }
        if (resource.role == ResourceRole::Scratch) {
            if ((resource.base & 3u) || (resource.permissions & 3u) != 3u)
                return make_issue(IssueKind::HostError,"InvalidScratch","Interpolation scratch must be aligned and readable/writable.");
            for (const auto& other : resources) if (&other != &resource && overlaps(resource,other))
                return make_issue(IssueKind::HostError,"ScratchOverlap","Host scratch must be disjoint from every other registered resource.");
        }
    }
    std::vector<Resource> validated(resources.begin(),resources.end());
    machine.resources=std::move(validated);
    return {};
}
Result<Resource> require_resource(const Machine& machine, ResourceRole role, Addr base, std::uint64_t bytes, Access access) {
    if (bytes != 0 && bytes <= (std::uint64_t{1}<<32)-base) {
        for (const auto& resource : machine.resources) {
            if (resource.role != role || !(resource.permissions & permission(access)) || !contains(resource,base,bytes)) continue;
            auto checked = machine.memory.check_access(base,bytes,access);
            if (!checked) return checked.error();
            return resource;
        }
    }
    auto issue = make_issue(IssueKind::ArchitecturalFault,"ResourceRange","Access is outside a registered resource of the required role/permission.");
    issue.address = base; issue.spec_issue_id = "S10.4"; return issue;
}
Result<void> check_submission_access(const Machine& machine, Addr base, std::uint64_t bytes, Access access) {
    if (machine.submission.status != SubmissionStatus::Busy) return {};
    if (access == Access::Write) {
        if (machine.submission.draw && machine.submission.draw->raster) {
            const auto record=machine.submission.draw->interpolation_address;
            const std::uint64_t length=12+12*machine.submission.graphics.varying_count;
            if (base < std::uint64_t{record}+length && record < std::uint64_t{base}+bytes) {
                auto issue=make_issue(IssueKind::ArchitecturalFault,"ReadOnlyInterpolation","The live PRIM_DATA_ADDR record is read-only to shader instructions.");
                issue.address=base; return issue;
            }
        }
        for (const auto& resource : machine.resources) {
            if (resource.role <= ResourceRole::Texture && base < std::uint64_t{resource.base}+resource.bytes && resource.base < std::uint64_t{base}+bytes) {
                auto issue=make_issue(IssueKind::ArchitecturalFault,"ResourcePermission","Guest input remains read-only even if another metadata view aliases it.");
                issue.address=base; issue.spec_issue_id="S10.4"; return issue;
            }
        }
    }
    for (const auto& resource : machine.resources) {
        if (access == Access::Execute && resource.role != ResourceRole::Shader) continue;
        if ((resource.permissions & permission(access)) && contains(resource,base,bytes)) return {};
    }
    auto issue = make_issue(IssueKind::ArchitecturalFault,"ResourcePermission","Shader access exceeds the submission's registered ranges/permissions.");
    issue.address = base; issue.spec_issue_id = "S10.4"; return issue;
}
} // namespace animus
