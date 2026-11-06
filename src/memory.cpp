#include "model/model.hpp"
#include <algorithm>
#include <cstdint>
#include <span>

namespace animus {
namespace {
bool valid_width(unsigned bytes) { return bytes == 1 || bytes == 2 || bytes == 4; }
Issue range_error() { return make_issue(IssueKind::ArchitecturalFault, "MemoryRange", "Address range is outside mapped memory."); }
}
Result<void> Memory::map_region(MemoryRegion region) {
    // Subtraction avoids overflow even when bytes is supplied by a malformed host case.
    if (region.bytes == 0 || region.base >= size() || region.bytes > size() - region.base)
        return range_error();
    if (region.permissions == 0 || (region.permissions & ~7u) != 0)
        return make_issue(IssueKind::HostError, "InvalidPermissions", "Permissions must use R/W/X bits.");
    const auto end = std::uint64_t{region.base} + region.bytes;
    for (const auto& old : regions_)
        if (region.base < std::uint64_t{old.base} + old.bytes && old.base < end)
            return make_issue(IssueKind::HostError, "RegionOverlap", "Memory mappings must not overlap.");
    regions_.push_back(region);
    return {};
}
Result<void> Memory::check_access(Addr address, std::uint64_t bytes, Access access) const {
    if (bytes == 0 || address >= size() || bytes > size() - address) return range_error();
    // Each access must fit within one explicitly mapped region.
    for (const auto& region : regions_) {
        if (address < region.base) continue;
        const auto offset = std::uint64_t{address} - region.base;
        if (offset <= region.bytes && bytes <= region.bytes - offset) {
            if ((region.permissions & permission(access)) != 0) return {};
            return make_issue(IssueKind::ArchitecturalFault, "MemoryPermission", "Memory access permission denied.");
        }
    }
    return range_error();
}
Result<Word> Memory::load_le(Addr address, unsigned bytes, Access access) const {
    if (!valid_width(bytes)) return make_issue(IssueKind::HostError, "InvalidWidth", "Use 1, 2 or 4 bytes.");
    auto checked = check_access(address, bytes, access);
    if (!checked) return checked.error();
    return peek_le_prechecked(address, bytes);
}
Result<void> Memory::store_le(Addr address, unsigned bytes, Word value) {
    if (!valid_width(bytes)) return make_issue(IssueKind::HostError, "InvalidWidth", "Use 1, 2 or 4 bytes.");
    auto checked = check_access(address, bytes, Access::Write);
    if (!checked) return checked.error();
    store_le_prechecked(address, bytes, value);
    return {};
}
Word Memory::peek_le_prechecked(Addr address, unsigned bytes) const {
    Word value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= Word{bytes_[address + i]} << (8 * i);
    return value;
}
void Memory::store_le_prechecked(Addr address, unsigned bytes, Word value) {
    for (unsigned i = 0; i < bytes; ++i) bytes_[address + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
Result<Word> Memory::fetch32(Addr pc) const {
    if ((pc & 3u) != 0) return make_issue(IssueKind::ArchitecturalFault, "InstructionAlignment", "PC must be 4-byte aligned.");
    return load_le(pc, 4, Access::Execute);
}
Result<CodeRegion> Memory::executable_region_at(Addr pc) const {
    auto checked = fetch32(pc);
    if (!checked) return checked.error();
    for (const auto& region : regions_)
        if (pc >= region.base && std::uint64_t{pc} + 4 <= std::uint64_t{region.base} + region.bytes)
            return CodeRegion{region.base, region.bytes};
    return range_error();
}
Result<void> Memory::load_image(Addr address, std::span<const std::uint8_t> bytes, unsigned permissions) {
    auto mapped = map_region({address, bytes.size(), permissions});
    if (!mapped) return mapped.error();
    std::copy(bytes.begin(), bytes.end(), bytes_.begin() + address);
    return {};
}
} // namespace animus
