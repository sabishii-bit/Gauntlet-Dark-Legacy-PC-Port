#pragma once

#include <string_view>

#include "engine/assets/ItemArchive.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {
/** A body's shadow: an archive's lone shadow object (a costume's SHADOWL1, a swarm kind's
 * SHADOW1L1..3L1) laid flat on the floor under the body, tilted to it, never writing depth.
 * It cannot move: the model refers to its own tree. */
class BlobShadow {
public:
    static constexpr f32 kLift = 0.1f; ///< over the floor it lies on (lbl_80347BE0)
    /** How much nearer the eye it is drawn, so that it sorts over the floor's own polygons as
     * the original's depth offset (zmod) does, where they stand a little off the collision. */
    static constexpr f32 kPull = 0.5f;

    BlobShadow() = default;
    ~BlobShadow() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(BlobShadow);

    /** Binds the named object of the archive; false, leaving it unbound, when there is none. */
    bool bind(RenderDevice& device, ItemArchive& archive, std::string_view object);
    bool bound() const { return m_model.bound(); }
    void clear();
    /** Where it lies: on `ground`, its up along the floor's `normal`, scaled by `size`. */
    static Mat4 placement(const Vec3& ground, const Vec3& normal, f32 size = 1.0f);
    /** `placement` drawn `kPull` nearer `eye` along the lines of sight, which leaves it where
     * it was on the screen. */
    static Mat4 pulledToward(const Mat4& placement, const Vec3& eye);
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& eye, const Vec3& ground,
              const Vec3& normal, const WorldLighting& lighting, f32 alpha, f32 size = 1.0f) const;

private:
    TreeInfo m_tree;
    TreeModel m_model;
};
} // namespace gdl::game
