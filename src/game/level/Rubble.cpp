#include "game/level/Rubble.h"

#include <algorithm>
#include <string>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/io/NativeStateDigest.h"

#include "game/level/NativeStateResources.h"

namespace gdl::game {

bool Rubble::appendPiecesDigest(NativeStateDigest& digest, std::span<const Piece> pieces) {
    digest.string("Rubble.native.1");
    digest.wide(pieces.size());
    for (const auto& piece : pieces) {
        const auto& [model, transform, archive, object] = piece;
        digest.resource(model.get());
        digest.values(transform, object);
        digest.resource(archive);
    }
    return digest.good();
}

bool Rubble::appendNativeDigest(NativeStateDigest& digest) const {
    // m_models retains immutable bindings from discarded branches. Only the
    // active native pieces are state; cache insertion order is not.
    return appendPiecesDigest(digest, m_pieces);
}

bool Rubble::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendPiecesDigest(digest, saved.m_pieces);
}

bool Rubble::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& binding : m_models) {
        const auto& [device, archive, object, model] = binding;
        static_cast<void>(device); // Backend allocation identity never selects gameplay.
        if ((archive == nullptr) || !model || !resources.refreshArchive(*archive) ||
            !resources.registerModel(*model, *archive, "rubble/" + object)) {
            return false;
        }
    }
    return resources.good();
}

void Rubble::saveLocal(LocalSnapshot& saved) const {
    saved.m_pieces = m_pieces;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
}

bool Rubble::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding;
}

bool Rubble::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_pieces == right.m_pieces;
}

bool Rubble::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_pieces = saved.m_pieces;
    return true;
}

bool Rubble::leave(RenderDevice& device, std::span<ItemArchive* const> archives,
                   std::string_view object, const Mat4& transform) {
    const auto hit = AssetScope(archives).findObject(object);
    if (!hit.has_value()) {
        return false;
    }
    ItemArchive* archive = hit->archive;
    const auto cached = std::ranges::find_if(m_models, [&](const ModelBinding& binding) {
        return binding.device == &device && binding.archive == archive &&
               binding.object == hit->name;
    });
    std::shared_ptr<const TreeModel> model;
    if (cached != m_models.end()) {
        model = cached->model;
    } else {
        TreeInfo tree;
        tree.name = std::string(object);
        TreeNodeInfo node;
        node.name = std::string(object);
        node.object = hit->name;
        tree.nodes.push_back(node);
        auto loaded = std::make_shared<TreeModel>();
        if (!loaded->bind(tree, archive->models, archive->textures, device)) {
            return false;
        }
        model = std::move(loaded);
        m_models.push_back({&device, archive, hit->name, model});
    }
    m_pieces.push_back({std::move(model), transform, archive, std::string{object}});
    return true;
}

std::vector<Rubble::Presentation> Rubble::presentation() const {
    std::vector<Presentation> result;
    for (usize index = 0; index < m_pieces.size(); ++index) {
        const auto& piece = m_pieces[index];
        result.push_back({index, piece.archive, piece.object, piece.transform});
    }
    return result;
}

void Rubble::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const Piece& piece : m_pieces) {
        piece.model->draw(device, clip, piece.transform, lighting);
    }
}

} // namespace gdl::game
