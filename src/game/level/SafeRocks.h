#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/TextureAnimator.h"

#include "game/critters/CombatEvents.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {

/** The boss arenas' type-10/subtype-41 cover. These are static models, not animation
 * trees: name + health tier, with the original L1 / L1ROOT lookup fallbacks. */
class SafeRocks {
public:
    static constexpr s32 kItemType = ItemInfo::kObstacle;
    static constexpr s32 kSubtype = 41;
    static constexpr s32 kWhole = 3;

    struct RockState {
        s32 instance = -1;
        s32 health = 0;
        s32 baseHealth = 0;
        s32 armor = 0;
        s32 tier = 0;
        s32 minPlayers = 0;
        bool shown = true;
        bool dormant = false;
        f32 activationDelay = 0;
        Vec3 position{0.0f};
        Obstacle obstacle;
        Mat4 placement{1.0f};
        bool operator==(const RockState&) const = default;
        template <class Visitor>
        friend void visitNativeState(Visitor& visitor, const RockState& value) {
            visitor.template fields<12>(value);
        }
    };
    struct Rock : RockState {
        std::array<TreeModel, 4> models;
    };

    /** Native values in the current bound arena; model and texture resources stay put. */
    class LocalSnapshot {
        friend class SafeRocks;
        std::vector<RockState> m_rocks;
        TextureAnimator::Clock m_textures;
        std::weak_ptr<const u8> m_binding;
        const SafeRocks* m_owner = nullptr;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources) const;

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items);
    /** Bind after the arena's lenders are loaded. K5's static rocks animate a texture
     * whose images live in PBOSS, not in the item archive. All archives must outlive us. */
    void bindAnimations(RenderDevice& device, ItemArchive& items,
                        std::span<TextureSet* const> lenders = {});
    void clear();
    void setPlayerCount(s32 players);
    usize size() const { return m_rocks.size(); }
    const Rock& rock(usize index) const { return *m_rocks[index]; }
    struct BodyResource {
        usize index = 0;
        s32 tier = 0;
        const TreeModel* model = nullptr;
        const TextureAnimator* textures = nullptr;
    };
    /** Loaded cover tiers and their shared texture clock, without advancing eruptions. */
    std::vector<BodyResource> bodyResources() const;
    f32 textureClock() const { return static_cast<f32>(m_textures.frame()); }
    bool standing(usize index) const;
    /** Armour reduces damage, with a minimum of one. True only on the destroying blow. */
    bool strike(usize index, f32 power);
    /** Restore all three health tiers, as the boss's reactivation does. */
    void activate(usize index);
    /** Eruption arenas start with invisible, non-solid rocks, not visible rubble. */
    void hideForEruptions();
    void scheduleActivation(usize index, f32 delay);
    /** A held simulation tick must not keep interpolating the last running tick. */
    void capturePresentation() { m_textures.advance(0); }
    void update(f32 seconds);
    std::vector<CombatArenaTarget> eruptionTargets() const;
    /** Complete visible roster, including active entries needed by retail's cycling selector. */
    std::vector<CombatArenaTarget> arenaTargets() const;
    std::vector<Obstacle> obstacles() const;
    /** Authored effect anchors include destroyed cover, not just standing obstacles. */
    std::vector<Mat4> attackAnchors() const;
    bool blocksBreath(const Vec3& from, const Vec3& to) const;
    bool blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f) const;

private:
    bool appendBindingDigest(NativeStateDigest& digest) const;
    friend class SafeRocksCheckpoint;
    std::vector<std::unique_ptr<Rock>> m_rocks;
    std::vector<u32> m_textureSlots;
    TextureAnimator m_textures;
    const WorldLayout* m_layout = nullptr;
    const ItemArchive* m_items = nullptr;
    const ItemArchive* m_animationItems = nullptr;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    u64 m_checkpointGeneration = 0;
};

} // namespace gdl::game
