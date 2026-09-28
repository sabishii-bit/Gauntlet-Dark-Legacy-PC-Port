#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/**
 * What destroyed things leave lying (MBOX_NewObject): one mesh of an item archive where the
 * thing stood, as it stood. Borrows the archives' meshes and textures; clear before they are
 * released.
 */
class Rubble {
public:
    static constexpr std::string_view kItem = "ITEMEXP0";    ///< a destroyed pickup's
    static constexpr std::string_view kChest = "CHESTGEXP0"; ///< a chest's
    static constexpr std::string_view kSilverChest = "CHESTSEXP0";
    static constexpr std::string_view kBlownBarrel = "BAREXP0"; ///< an exploding barrel's
    static constexpr std::string_view kGasBarrel = "BARPOI0";   ///< a poison barrel's

    /** Leaves `object`, the first of `archives` to hold it, placed by `transform`; false when
     * none does. */
    bool leave(RenderDevice& device, std::span<ItemArchive* const> archives,
               std::string_view object, const Mat4& transform);
    void clear() { m_pieces.clear(); }
    usize size() const { return m_pieces.size(); }
    const Mat4& transform(usize index) const { return m_pieces[index]->transform; }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    struct Piece {
        TreeModel model;
        Mat4 transform{1.0f};
    };
    std::vector<std::unique_ptr<Piece>> m_pieces;
};

} // namespace gdl::game
