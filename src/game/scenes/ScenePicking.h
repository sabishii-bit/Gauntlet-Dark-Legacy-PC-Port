#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/party/PlayerRuntime.h"
#include "game/scenes/SceneSelection.h"

namespace gdl {
struct Mesh;
class SelectionHighlight;
struct WorldCamera;
} // namespace gdl

namespace gdl::game {

class LevelWorld;
class LevelOpponents;
class LevelFixtures;
class SpawnedObjects;

/**
 * The developer console's selection: casts the cursor's ray against everything the scene
 * shows (the level's objects by their faces, the swarm, the critters, the generators, the
 * pickups, the switches, the gates, the chests, the party and the console's own objects),
 * holds what was picked, and marks it out in the view by handing the highlight to whichever
 * subsystem owns the thing selected.
 */
class ScenePicking {
public:
    /** What the scene shows, borrowed for one call. */
    struct Scene {
        LevelWorld* world = nullptr;
        LevelOpponents& opponents;
        const LevelFixtures& fixtures;
        std::span<const PlayerRuntime> players;
        SpawnedObjects& spawned;
    };

    /** A ray from the eye, unit length. */
    struct Ray {
        Vec3 origin{0.0f};
        Vec3 direction{0.0f, 0.0f, 1.0f};
    };

    /** The nearest thing under `cursor` (0 to 1 across and down the view) as last rendered
     * under `clip` and seen from `view`; nothing with nothing there. */
    static std::optional<SceneSelection> pick(const Scene& scene, Vec2 cursor, const Mat4& clip,
                                              const WorldCamera& view, f32 worldAlpha);

    /** The selection the console holds, marked out while it holds. */
    void select(const std::optional<SceneSelection>& selection) { m_selection = selection; }
    const std::optional<SceneSelection>& selection() const { return m_selection; }

    /** Hands `highlight` to whichever subsystem owns the selection; null takes it back. */
    void markSelection(const Scene& scene, const SelectionHighlight* highlight) const;

    /** The ray from the eye through `cursor` (0 to 1 across and down the view) under `clip`. */
    static std::optional<Ray> cursorRay(Vec2 cursor, const Mat4& clip, const Vec3& eye);
    /** How far along the ray it enters the sphere; nought from inside it; nothing when it
     * misses. */
    static std::optional<f32> sphereHit(const Ray& ray, const Vec3& centre, f32 radius);
    /** How far along the ray it meets the triangle, either side; nothing when it misses. */
    static std::optional<f32> triangleHit(const Ray& ray, const Vec3& a, const Vec3& b,
                                          const Vec3& c);
    /** The nearest of the mesh's triangles along the ray, placed by `placement`. */
    static std::optional<f32> meshHit(const Ray& ray, const Mesh& mesh, const Mat4& placement);

private:
    std::optional<SceneSelection> m_selection;
};

} // namespace gdl::game
