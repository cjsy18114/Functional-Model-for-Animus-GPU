#include "model/model.hpp"
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <vector>

namespace animus {
Result<void> load_binary(Machine& machine, const BinaryImage& image) {
    std::ifstream input(image.path, std::ios::binary | std::ios::ate);
    if (!input) return make_issue(IssueKind::HostError, "FileOpen", "Cannot open binary image.");
    const auto length = input.tellg();
    if (length <= 0 || (length % 4) != 0 || (image.load_address & 3u) != 0 || (image.entry & 3u) != 0)
        return make_issue(IssueKind::HostError, "InvalidImage", "Require a nonempty, word-aligned code image and entry.");
    const auto count = static_cast<std::uint64_t>(length);
    if (image.load_address >= machine.memory.size() || count > machine.memory.size() - image.load_address ||
        image.entry < image.load_address || std::uint64_t{image.entry} + 4 > std::uint64_t{image.load_address} + count)
        return make_issue(IssueKind::HostError, "ImageRange", "Image or entry does not fit in model memory.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return make_issue(IssueKind::HostError, "FileRead", "Cannot read complete binary image.");
    // Code is readable/executable, never guest-writable. Raw bins contain code only in A.
    return machine.memory.load_image(image.load_address, bytes, permission(Access::Read) | permission(Access::Execute));
}
} // namespace animus
