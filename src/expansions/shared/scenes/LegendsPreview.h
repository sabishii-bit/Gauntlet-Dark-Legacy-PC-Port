#pragma once

#include <map>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"

#include "expansions/n64/camera/LegendsCamera.h"
#include "expansions/n64/scenes/LegendsWorldPresentation.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/camera/FreeCamera.h"
#include "game/characters/Party.h"
#include "game/hud/StatusBox.h"
#include "game/level/ImportedWorldObject.h"
#include "game/party/PlayerFigure.h"
#include "game/players/CursorAim.h"

namespace gdl::game {

/** Offline native-map inspection and local gameplay-adapter testing. */
class LegendsPreview {
public:
    /** Builds a replacement before discarding the current preview. Throws on invalid geometry. */
    void open(RenderDevice& device, const formats::LegendsMap& map,
              std::unique_ptr<LegendsSceneAdapter> adapter = {});
    /** Walk with a visiting character; the caller commits pickups on return. */
    void openPlayable(RenderDevice& device, const formats::LegendsMap& map,
                      const std::filesystem::path& characterRoot, const PartyMember& member,
                      std::unique_ptr<LegendsSceneAdapter> adapter = {});
    void close();
    bool isOpen() const { return m_camera.active(); }
    usize triangleCount() const { return m_triangleCount; }
    bool playable() const { return m_traversal != nullptr; }
    const LegendsTraversal* traversal() const { return m_traversal.get(); }
    bool transferProgress(CharacterSave& live);
    const WorldCamera& camera() const { return playable() ? m_followCamera : m_camera.camera(); }
    LegendsSceneAdapter* adapter() { return m_adapter.get(); }
    const LegendsSceneAdapter* adapter() const { return m_adapter.get(); }
    std::optional<ImportedWorldObject> object(s32 id) const;
    std::optional<ImportedObjectHit> pickObject(const Vec3& origin, const Vec3& direction,
                                                const WorldCamera& view, f32 alpha) const;
    void highlightObject(RenderDevice& device, const Mat4& clip, const WorldCamera& view, f32 alpha,
                         s32 id, const SelectionHighlight& highlight) const;
    /** Native geometry in the ordinary gameplay render phases; no camera, player or HUD. */
    void drawWorld(RenderDevice& device, const Mat4& transform, const WorldCamera& view,
                   TreeModel::Pass pass, f32 presentationAlpha) const;
    void update(const Input& input, f32 seconds, bool consoleCapturesInput);
    void updatePlayable(const Input& input, f32 seconds, bool consoleCapturesInput,
                        const GameConfig& config);
    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight,
                f32 horizontalFov) const;

private:
    struct Batch {
        ImmediateBatch geometry;
        u32 texture = 0;
        usize object = 0; ///< native world identity, retained for scripted motion and UVs
        Vec3 centre{0.0f};
        LegendsWorldPresentation presentation;
        bool hasNativeDrawState = false;
        Mat4 authoredPlacement{1};
        Mat4 inversePlacement{1};
        bool cullBack = false;
        u32 lightmap = 0;
        u32 dreamcastFacing = 0;
        bool additive = false;
    };
    struct Surface {
        std::unique_ptr<Texture> texture;
        bool hasTransparency = false;
        bool softAlpha = false;
    };
    Mat4 batchPlacement(const Batch& batch, const WorldCamera& view, f32 alpha) const;
    bool batchVisible(const Batch& batch) const;
    std::vector<Batch> m_batches;
    std::vector<ImportedWorldObject> m_objects;
    std::vector<Surface> m_surfaces;
    std::map<u32, std::unique_ptr<Texture>> m_lightmaps;
    std::unique_ptr<LegendsSceneAdapter> m_adapter;
    usize m_triangleCount = 0;
    FreeCamera m_camera;
    f32 m_far = WorldCamera::kFar;
    std::unique_ptr<LegendsTraversal> m_traversal;
    std::unique_ptr<PlayerFigure> m_figure;
    std::unique_ptr<StatusBoxPainter> m_status;
    CharacterSave m_before;
    f32 m_powerupTime = 1;
    WorldCamera m_followCamera;
    std::optional<LegendsCamera> m_nativeCamera;
    Vec3 m_previousPosition{0};
    f32 m_previousYaw = 0;
    f64 m_elapsed = 0;
    CursorInput m_cursorInput;
    mutable Mat4 m_lastClip{1};
    mutable bool m_hasClip = false;
};

} // namespace gdl::game
