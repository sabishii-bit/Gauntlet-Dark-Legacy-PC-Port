#include "expansions/n64/level/LegendsPortalAppearance.h"

#include <string_view>

namespace gdl::game {
namespace {
bool objectPrefix(const TreeNodeInfo& node, std::string_view prefix) {
    // Tower e0863d24 calls e001cfc0, which reads the OBHD name plus its
    // prefix-byte offset. The decoder preserves that native part alias in
    // node.name; node.object is the full archive name (EXIT_ACTIVE1CENTEREXIT)
    // and may additionally be prefixed by a generated texture variant.
    return node.name.starts_with(prefix);
}
} // namespace

std::vector<usize> legendsLockedPortalMeshes(const TreeInfo& tree) {
    std::vector<usize> hidden;
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        bool suppress = objectPrefix(tree.nodes[i], "CENTER");
        s32 current = static_cast<s32>(i);
        for (usize depth = 0; !suppress && depth < tree.nodes.size(); ++depth) {
            if (current < 0 || static_cast<usize>(current) >= tree.nodes.size()) {
                break;
            }
            const auto& node = tree.nodes[static_cast<usize>(current)];
            suppress = objectPrefix(node, "XCOANI");
            current = node.parent;
        }
        if (suppress) {
            hidden.push_back(i);
        }
    }
    return hidden;
}

} // namespace gdl::game
