#pragma once

#include <map>
#include <span>

#include "game/party/TurboEmission.h"

namespace gdl::game {
class PlayerFigure;

/** Load-time native B/C effect catalog for one selected costume. This is resource
 * preparation, not permission to predict hits or a complete network presentation
 * lane. Archives, class data and all lenders must outlive this catalog and copies
 * of its bound prototypes. Lookups never bind, load, animate or emit anything. */
class TurboEffectResources {
public:
    enum class Status : u8 {
        NotTurbo,
        Ready,
        InvalidChain,
        UnsupportedStrike,
        UnsupportedParent,
        MissingResource,
    };
    enum class Kind : u8 { Invisible, Tree, Particles };
    struct Resource {
        u32 id = 0; ///< SFXX row + 1, local to this selected player's catalog
        s32 row = -1;
        Kind kind = Kind::Invisible;
        ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::vector<TextureSet*> lenders;
        TreeModel model;
        TreeParticles particles;
        TextureAnimator textures;
        std::optional<ParticleDescriptor> descriptor;
        std::optional<u32> textureSlot;
        const Texture* texture = nullptr;
        /** Requires the full particle replay/confirmation lane, not mesh-only replication. */
        bool requiresParticles() const {
            return kind == Kind::Particles || particles.field().size() != 0;
        }
    };
    struct Strike {
        s32 row = -1;
        f32 effectSeconds = 0;
        std::vector<u32> resources; ///< complete native chained SFXX order
    };

    bool bind(RenderDevice& device, s32 character, const ClassStats& stats, PlayerFigure& figure,
              ItemArchive& weapons, std::span<TextureSet* const> lenders = {});
    /** Explicit trusted archive form, also usable without a live actor at load time. */
    bool bind(RenderDevice& device, s32 character, const ClassStats& stats, ItemArchive& effects,
              ItemArchive& weapons, std::span<TextureSet* const> lenders = {});
    void clear();
    const ClassStats* stats() const { return m_stats; }
    s32 character() const { return m_character; }
    Status status(s32 row) const;
    const Strike* strike(s32 row) const;
    const Resource* resource(u32 id) const;
    /** Uses the same native descriptor construction as authoritative combat. */
    std::optional<TurboEmission::Strike> prepare(s32 row, const TurboEmission::Context& context,
                                                 f32 meleeDamage) const;

private:
    static constexpr usize kMaxRows = 1024;
    Status prepareStrike(RenderDevice& device, s32 row, ItemArchive& effects, ItemArchive& weapons,
                         std::span<TextureSet* const> lenders);
    static std::optional<Resource> bindResource(RenderDevice& device, s32 row,
                                                const MoveEffect& effect, ItemArchive& effects,
                                                ItemArchive& weapons,
                                                std::span<TextureSet* const> lenders);
    s32 m_character = -1;
    const ClassStats* m_stats = nullptr;
    std::map<s32, Status> m_status;
    std::map<s32, Strike> m_strikes;
    std::map<u32, Resource> m_resources;
};
} // namespace gdl::game
