#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/LocalBinding.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/LocalState.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

class NativeStateResources;

/**
 * What destroyed things leave lying (MBOX_NewObject): one mesh of an item archive where the
 * thing stood, as it stood. Borrows the archives' meshes and textures; clear before they are
 * released.
 */
class Rubble : public LocalState<Rubble> {
private:
    struct Piece {
        std::shared_ptr<const TreeModel> model;
        Mat4 transform{1.0f};
        const ItemArchive* archive = nullptr;
        std::string object;
        bool operator==(const Piece&) const = default;
    };

public:
    Rubble() = default;
    Rubble(const Rubble&) = delete;
    Rubble& operator=(const Rubble&) = delete;
    Rubble(Rubble&&) noexcept = default;
    Rubble& operator=(Rubble&&) noexcept = default;
    ~Rubble() = default;

    static constexpr std::string_view kItem = "ITEMEXP0";    ///< a destroyed pickup's
    static constexpr std::string_view kChest = "CHESTGEXP0"; ///< a chest's
    static constexpr std::string_view kSilverChest = "CHESTSEXP0";
    static constexpr std::string_view kBlownBarrel = "BAREXP0"; ///< an exploding barrel's
    static constexpr std::string_view kGasBarrel = "BARPOI0";   ///< a poison barrel's

    bool registerNativeResources(NativeStateResources& resources) const;

    /** Leaves `object`, the first of `archives` to hold it, placed by `transform`; false when
     * none does. */
    bool leave(RenderDevice& device, std::span<ItemArchive* const> archives,
               std::string_view object, const Mat4& transform);
    void clear() {
        m_pieces.clear();
        m_models.clear();
        m_localBinding.rebind();
    }
    usize size() const { return m_pieces.size(); }
    const Mat4& transform(usize index) const { return m_pieces[index].transform; }
    struct Presentation {
        usize index = 0;
        const ItemArchive* archive = nullptr;
        std::string_view object;
        Mat4 placement{1};
    };
    /** Append-only identities and trusted artwork for already-created debris. */
    std::vector<Presentation> presentation() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    friend class LocalState<Rubble>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    /** The pieces lying, as a snapshot keeps them: the archives and device they were bound
     * through must outlive the history. */
    using LocalValues = std::vector<Piece>;
    struct LocalChildren {};
    static constexpr auto localParts() { return local::parts(local::own()); }
    const LocalValues& localValues() const { return m_pieces; }
    LocalValues& localValues() { return m_pieces; }
    /** Writes the schema and `pieces`. */
    static bool digestLocalValues(NativeStateDigest& digest, std::span<const Piece> pieces);
    struct ModelBinding {
        const RenderDevice* device = nullptr;
        const ItemArchive* archive = nullptr;
        std::string object;
        std::shared_ptr<const TreeModel> model;
    };
    std::vector<Piece> m_pieces;
    std::vector<ModelBinding> m_models;
    LocalBinding m_localBinding;
};

} // namespace gdl::game
