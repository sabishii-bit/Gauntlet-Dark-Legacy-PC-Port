#include "game/world/Rubble.h"

#include <string>

namespace gdl::game {

bool Rubble::leave(RenderDevice& device, std::span<ItemArchive* const> archives,
                   std::string_view object, const Mat4& transform) {
    for (ItemArchive* archive : archives) {
        if (archive == nullptr || !archive->models.find(object).has_value()) {
            continue;
        }
        TreeInfo tree;
        tree.name = std::string(object);
        TreeNodeInfo node;
        node.name = std::string(object);
        node.object = std::string(object);
        tree.nodes.push_back(node);
        auto piece = std::make_unique<Piece>();
        if (!piece->model.bind(tree, archive->models, archive->textures, device)) {
            return false;
        }
        piece->transform = transform;
        m_pieces.push_back(std::move(piece));
        return true;
    }
    return false;
}

void Rubble::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const std::unique_ptr<Piece>& piece : m_pieces) {
        piece->model.draw(device, clip, piece->transform, lighting);
    }
}

} // namespace gdl::game
