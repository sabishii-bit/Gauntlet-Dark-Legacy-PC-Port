#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/ParticleField.h"

namespace gdl {
/** Authored particle nodes of an animation tree. The archive owns their textures;
 * each emitter follows its posed node, while particle lifetimes follow its template. */
class TreeParticles {
public:
    void bind(const TreeInfo& tree, ItemArchive& archive, RenderDevice& device, const Mat4& root,
              std::span<const Mat4> pose);
    void step(f32 seconds, const Mat4& root, std::span<const Mat4> pose);
    void setTextureFrame(u32 slot, const Texture& texture);
    /** Ends emission without discarding particles already in flight. */
    void stop();
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& right, const Vec3& up) const;
    const ParticleField& field() const { return m_field; }

private:
    ParticleField m_field;
    struct Binding {
        usize node = 0;
        std::optional<u32> texture;
    };
    std::vector<Binding> m_nodes; ///< emitter index -> posed tree node and texture slot
};
} // namespace gdl
