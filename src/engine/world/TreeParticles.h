#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/ParticleField.h"
#include "engine/world/ThermalMaterial.h"
#include "engine/world/TreePose.h"

namespace gdl {
/** Authored particle nodes of an animation tree. The archive owns their textures;
 * each emitter follows its posed node, while particle lifetimes follow its template. */
class TreeParticles {
public:
    bool equalLocal(const TreeParticles& other) const {
        return m_nodes == other.m_nodes && m_field.equalLocal(other.m_field);
    }
    bool operator==(const TreeParticles& other) const { return equalLocal(other); }
    struct Binding {
        usize node = 0;
        std::optional<u32> texture;
        bool operator==(const Binding&) const = default;
    };
    struct Checkpoint {
        ParticleField::Checkpoint field;
        std::vector<Binding> nodes;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    Checkpoint checkpoint() const;
    bool canRestore(const Checkpoint& state) const;
    bool restore(const Checkpoint& state);
    void bind(const TreeInfo& tree, ItemArchive& archive, RenderDevice& device, const Mat4& root,
              std::span<const Mat4> pose, std::span<TextureSet* const> lenders = {});
    void step(f32 seconds, const Mat4& root, std::span<const Mat4> pose);
    void setEmitting(bool emitting);
    void setLocalScales(std::span<const NodePose> poses);
    void setTextureFrame(u32 slot, const Texture& texture);
    /** Render-only sprite sampling; clear before preparing a new draw's texture clock. */
    void setTextureBlend(u32 slot, const Texture& current, const Texture* next, f32 blend,
                         bool stepped = false);
    void clearTextureBlends();
    /** Ends emission without discarding particles already in flight. */
    void stop();
    /** The emitters facing the camera, as ParticleField::draw; those wearing one of
     * `thermal`'s particle textures add heat. */
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& right, const Vec3& up,
              f32 presentationAlpha, const ThermalMaterial& thermal) const;
    const ParticleField& field() const { return m_field; }

private:
    friend void visitNativeState(NativeStateDigest& /*digest*/, const TreeParticles& /*state*/);
    ParticleField m_field;
    std::vector<Binding> m_nodes; ///< emitter index -> posed tree node and texture slot
};
} // namespace gdl
