#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"

#include "game/camera/TowerCamera.h"

namespace gdl::game {

/** Paired TRANS items move individual players within a level, not through its exits. */
class LevelTransporters {
public:
    LevelTransporters() = default;
    LevelTransporters(const LevelTransporters&) = delete;
    LevelTransporters& operator=(const LevelTransporters&) = delete;
    LevelTransporters(LevelTransporters&&) = delete;
    LevelTransporters& operator=(LevelTransporters&&) = delete;
    ~LevelTransporters() = default;
    static constexpr s32 kItemType = ItemInfo::kTransporter;
    struct Contact {
        s32 instance = -1;
        s32 id = 0;
        s32 destinationId = 0;
        std::optional<usize> destination;
        Vec3 position{0.0f};
        f32 radius = 0.0f;
        f32 height = 0.0f;
        Mat4 transform{1.0f};
        s32 support = -1;        ///< the world object it stands on, when that one moves
        Mat4 supportLocal{1.0f}; ///< its art, in the support's own space
        Vec3 anchorLocal{0.0f};  ///< its contact and landing point, likewise
        void sync(const WorldCollision& collision);
        bool touches(const Vec3& feet, f32 bodyRadius, f32 bodyHeight) const;
    };
    /** The same load-time support binding drives native pads and transactional
     * travel-boundary checks; sync the value at the native floor-carry phase. */
    static std::vector<Contact> contacts(const WorldLayout& layout, s32 players,
                                         const WorldCollision* collision = nullptr);
    struct PadState : Contact {
        TreePose pose;
        AnimationPlayer animation;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
    };
    struct Pad : PadState {
        mutable TreeModel model;
    };
    class LocalSnapshot {
        friend class LevelTransporters;
        std::vector<PadState> m_pads;
        TextureAnimator::Clock m_textures;
        bool m_presentationAdvanced = false;
        const LevelTransporters* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    /** Read-only draw anchors. Transforms use the current carried floor pose;
     * only the animation and shared texture clock interpolate. */
    struct Presentation {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        u32 instance = 0;
        Mat4 placement{1};
        u32 sequence = 0;
        u64 generation = 0;
        f32 frame = 0, previousFrame = 0;
        f32 textureClock = 0, previousTextureClock = 0;
        bool playing = false, continuous = false, advanced = false;
    };
    std::optional<Presentation> presentation(usize index) const;

    /** The scope's archives must outlive the pads; the first holding TRANS supplies them. A
     * pad on a moving floor is carried by it (AddItemSub parents it to the floor's node). */
    void bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope, s32 players,
              const WorldCollision* collision = nullptr);
    void clear();
    /** Carries the pads with their moving floors: art, contact and landing point alike. */
    void syncFloors(const WorldCollision& collision);
    void animate(f32 seconds);
    void capturePresentation() {
        m_presentationAdvanced = false;
        m_textures.advance(0);
    }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 renderAlpha = -1.0f) const;
    usize size() const { return m_pads.size(); }
    const Pad& pad(usize index) const { return m_pads[index]; }
    std::optional<usize> contact(const Vec3& feet, f32 radius, f32 height) const;
    /** Reject missing links, offscreen destinations and landings with no floor. */
    std::optional<Vec3> landing(usize source, f32 radius, const WorldCollision& collision,
                                const WorldCamera& camera, const CameraView& view) const;
    static bool visible(const Vec3& position, f32 radius, const WorldCamera& camera,
                        const CameraView& view);
    static std::string_view soundForRealm(s32 realm);

private:
    void appendBinding(NativeStateDigest& digest) const;
    static void appendPad(NativeStateDigest& digest, const PadState& pad);
    std::vector<Pad> m_pads;
    const TreeInfo* m_tree = nullptr;
    TextureAnimator m_textures;
    bool m_presentationAdvanced = false;
    const WorldLayout* m_layout = nullptr;
    ItemArchive* m_archive = nullptr;
    const WorldCollision* m_collision = nullptr;
    s32 m_players = 1;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    u64 m_checkpointGeneration = 0;
};

} // namespace gdl::game
