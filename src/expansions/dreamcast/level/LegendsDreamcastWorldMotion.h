#pragma once

#include <array>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/world/WorldCollision.h"

#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"
#include "expansions/shared/level/LegendsFixtureEvents.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"

namespace gdl::game {

/** Dreamcast world-node animation and native type5 contacts. Identity is the
 * authored world table, not an inferred N64 fixture or a presentation mesh. */
class LegendsDreamcastWorldMotion {
public:
    struct Switch {
        u32 nativeIndex = 0;
        s32 target = -1;
        u32 flags = 0;
        u8 id = 0, nextId = 0;
        std::string name;
        Vec3 position{0};
        Vec3 rotation{0};
        f32 radius = 0, height = 0;
        s32 floor = -1;
        s32 next = -1;
        bool hidden = false, active = false;
        f32 cooldown = 0;
    };
    LegendsDreamcastWorldMotion() = default;
    LegendsDreamcastWorldMotion(const formats::LegendsMap& authored,
                                const formats::LegendsMap& initial,
                                const formats::LegendsSceneMotion& motion,
                                const formats::LegendsDreamcastScene& scene, s32 players);
    void capturePresentation();
    void advance(f32 seconds);
    void touch(std::span<const LegendsPortalVisitor> visitors);
    void support(std::span<const s32> owners);
    void bindCollision(WorldCollision& collision) const;
    void syncCollision(WorldCollision& collision) const;
    Mat4 transform(usize object, f32 alpha = -1) const;
    Mat4 riderTransform(s32 owner) const;
    Vec3 offset(usize object, f32 alpha = -1) const;
    f32 opacity(usize object, f32 alpha = -1) const;
    /** AddItemSub grounds eligible native items after target initialization. */
    Vec3 itemPosition(const formats::ItemInfoRecord& info, Vec3 authored) const;
    std::optional<FloorHit> supportAt(Vec3 position) const;
    bool carriesItems(s32 object) const;
    bool moves(s32 object) const;
    /** Current native triggerstate0x10 on the support or its immediate parent. */
    bool activatesBoss(s32 support) const;
    std::optional<std::pair<usize, Vec3>> ambientParent(Vec3 position, f32 radius = 10) const;
    std::span<const Switch> switches() const { return m_switches; }
    Vec3 switchPosition(usize index) const;
    using SoundResolver = std::function<std::optional<std::array<u32, 2>>(s32)>;
    void bindSounds(const SoundResolver& resolve);
    std::vector<LegendsSoundLoop> soundLoops() const;
    std::vector<LegendsSoundEvent> takeSounds();
    const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    struct Track {
        formats::WorldAnimationRecord definition;
        f32 frame = 0;
        bool running = false, reverse = false, once = false;
    };
    struct Target {
        s32 object = -1;
        s32 subtype = 0;
        s8 soundSlot = -1; ///< the authored slot byte, signed: a negative slot sounds nothing
        std::optional<std::array<u32, 2>> sounds;
        u32 flags = 0;
        f32 closed = 0, opened = 0, height = 0;
        f32 alpha = 1, previousAlpha = 1;
        bool open = false, moving = false, animated = false, pressed = false;
    };
    struct Lightning {
        usize object = 0;
        f32 remaining = 0;
        bool visible = false;
    };
    void compose();
    void fire(Target& target, bool open);
    bool descendant(s32 object, s32 ancestor) const;
    Target* targetOf(s32 object);
    std::vector<formats::WorldObjectRecord> m_objects;
    std::vector<s32> m_parents;
    std::vector<Track> m_tracks;
    std::vector<Target> m_targets;
    std::vector<Lightning> m_lightning;
    std::minstd_rand m_lightningRandom{0x4c4e494e};
    std::vector<Switch> m_switches;
    std::vector<Mat4> m_world, m_previous, m_initialInverse, m_bindingInverse;
    std::vector<bool> m_moving;
    std::vector<s32> m_collisionOwners, m_support;
    WorldCollision m_bindingFloors;
    std::vector<std::string> m_warnings;
    std::vector<LegendsSoundEvent> m_soundEvents;
};

} // namespace gdl::game
