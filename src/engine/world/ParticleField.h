#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/ParticleSystem.h"

namespace gdl {

/**
 * A level's particle systems: one emitter at every marker of the layout, started from the
 * template the marker's name picks ("PSYS" then the template's letter), stepped thirty
 * frames a second and drawn facing the camera after the geometry.
 */
class ParticleField {
public:
    static constexpr std::string_view kTag = "PSYS";
    static constexpr f32 kFrameRate = ParticleDescriptor::kFrameRate;

    /** Starts an emitter per marker; textures are found by name in the level's set, then
     * in `lenders`. Markers naming no template are skipped with a warning. */
    void bind(const WorldLayout& layout, TextureSet& textures, RenderDevice& device,
              std::span<TextureSet* const> lenders = {}, u32 seed = 1);
    void clear();
    usize size() const { return m_entries.size(); }
    const ParticleEmitter& emitter(usize index) const { return m_entries[index].emitter; }
    const Texture* textureOf(usize index) const { return m_entries[index].texture; }
    /** Live particles over every emitter. */
    usize particleCount() const;
    /** Starts an emitter of its own at `node`, apart from any layout; the index addresses
     * it until prune() drops it. */
    usize start(const ParticleDescriptor& descriptor, const Mat4& node, const Texture* texture,
                u32 seed = 1);
    /** Moves an emitter's marker; new particles leave from there. */
    void setNode(usize index, const Mat4& node);
    void setEmitting(usize index, bool emitting);
    void setSpriteScale(usize index, f32 scale);
    /** Replaces a sprite frame without restarting its emitter or live particles. */
    void setTexture(usize index, const Texture& texture);
    /** Render-only flipbook pair. Native texture bindings and emission remain unchanged. */
    void setTextureBlend(usize index, const Texture& current, const Texture* next, f32 blend);
    void clearTextureBlends();
    /** Ends an emitter's emission; its particles live out their time. */
    void stop(usize index);
    bool active(usize index) const { return m_entries[index].emitter.active(); }
    /** Drops emitters with nothing left to show. */
    void prune();

    /** Moves `seconds` on, in whole game frames. */
    void step(f32 seconds);
    /** Camera-facing particles. Negative alpha draws native ticks; [0,1] samples the
     * last simulation interval without advancing emission. Held owners pass -1. */
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& right, const Vec3& up,
              f32 presentationAlpha = -1.0f) const;

private:
    struct Entry {
        ParticleEmitter emitter;
        const Texture* texture = nullptr;
        DrawState state;
        const Texture* presentedTexture = nullptr;
        const Texture* nextTexture = nullptr;
        f32 textureBlend = 0.0f;
    };

    std::vector<Entry> m_entries;
    f32 m_frameRemainder = 0.0f;
    f32 m_lastAdvance = 0.0f;
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
