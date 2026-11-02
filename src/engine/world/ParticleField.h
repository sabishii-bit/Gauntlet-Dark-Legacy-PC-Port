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
#include "engine/world/ThermalMaterial.h"

namespace gdl {

class WorldScene;

/**
 * A level's particle systems: one emitter at every marker of the layout, started from the
 * template the marker's name picks ("PSYS" then the template's letter), stepped thirty
 * frames a second and drawn facing the camera after the geometry.
 */
class ParticleField {
public:
    ParticleField() = default;
    /** Native values and borrowed textures only; generated draw vertices are scratch. */
    ParticleField(const ParticleField& other) : m_state(other.m_state) {}
    ParticleField& operator=(const ParticleField& other) {
        if (this != &other) {
            m_state = other.m_state;
            m_batch.clear();
        }
        return *this;
    }
    ParticleField(ParticleField&&) noexcept = default;
    ParticleField& operator=(ParticleField&&) noexcept = default;
    ~ParticleField() = default;
    /** Exact native values under the same borrowed texture bindings. Generated
     * vertices and per-draw flipbook samples are deliberately excluded. */
    bool equalLocal(const ParticleField& other) const;
    bool operator==(const ParticleField& other) const { return equalLocal(other); }
    static constexpr std::string_view kTag = "PSYS";
    static constexpr f32 kFrameRate = ParticleDescriptor::kFrameRate;

    struct Checkpoint {
        struct Entry {
            ParticleEmitter::Checkpoint emitter;
            s32 worldObject = -1;
            bool operator==(const Entry&) const = default;
        };
        std::vector<Entry> entries;
        f32 frameRemainder = 0;
        f32 lastAdvance = 0;
        f64 heatFrames = 0;
        /** Values only: an authored level may own more than the diagnostic wire's 256 emitters. */
        bool validValues() const;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    Checkpoint checkpoint() const;
    bool canRestore(const Checkpoint& state) const;
    /** Keeps prebound textures and draw policy; topology and descriptors must match. */
    bool restore(const Checkpoint& state);

    /** Starts an emitter per marker; textures are found by name in the level's set, then
     * in `lenders`. Markers naming no template are skipped with a warning. */
    void bind(const WorldLayout& layout, TextureSet& textures, RenderDevice& device,
              std::span<TextureSet* const> lenders = {}, u32 seed = 1);
    void clear();
    usize size() const { return m_state.entries.size(); }
    const ParticleEmitter& emitter(usize index) const { return m_state.entries[index].emitter; }
    const Texture* textureOf(usize index) const { return m_state.entries[index].texture; }
    /** Authored layout marker identity, independent of texture batching order.
     * Independently started effects have no layout marker and return -1. */
    s32 sourceObject(usize index) const { return m_state.entries[index].sourceObject; }
    /** Live particles over every emitter. */
    usize particleCount() const;
    /** Starts an emitter of its own at `node`, apart from any layout; the index addresses
     * it until prune() drops it. */
    usize start(const ParticleDescriptor& descriptor, const Mat4& node, const Texture* texture,
                u32 seed = 1);
    /** Moves an emitter's marker; new particles leave from there. */
    void setNode(usize index, const Mat4& node);
    /** Refreshes animated world markers before emission. Live particles retain their
     * birth positions and velocities; static markers and independent effects are unchanged. */
    void syncNodes(const WorldScene& scene);
    void setEmitting(usize index, bool emitting);
    void setSpriteScale(usize index, f32 scale);
    /** Replaces a sprite frame without restarting its emitter or live particles. */
    void setTexture(usize index, const Texture& texture);
    /** Render-only flipbook pair. A stepped sample also replaces the drawn native
     * frame for paused/cutout draws, without changing the native texture binding. */
    void setTextureBlend(usize index, const Texture& current, const Texture* next, f32 blend,
                         bool stepped = false);
    void clearTextureBlends();
    /** Ends an emitter's emission; its particles live out their time. */
    void stop(usize index);
    bool active(usize index) const { return m_state.entries[index].emitter.active(); }
    /** Drops emitters with nothing left to show. */
    void prune();

    /** Moves `seconds` on, in whole game frames. */
    void step(f32 seconds);
    /** Camera-facing particles. Negative alpha draws native ticks; [0,1] samples the
     * last simulation interval without advancing emission. Held owners pass -1. Emitters
     * wearing one of `thermal`'s particle textures add heat where they burn; an empty
     * material adds none. */
    void draw(RenderDevice& device, const Mat4& clip, const Vec3& right, const Vec3& up,
              f32 presentationAlpha, const ThermalMaterial& thermal) const;

private:
    friend void visitNativeState(NativeStateDigest& /*digest*/, const ParticleField& /*state*/);
    struct Entry {
        ParticleEmitter emitter;
        s32 sourceObject = -1;
        s32 worldObject = -1; ///< animated source marker, kept with its batched emitter
        const Texture* texture = nullptr;
        DrawState state;
        const Texture* presentedTexture = nullptr;
        const Texture* nextTexture = nullptr;
        f32 textureBlend = 0.0f;
        bool steppedTexture = false;
    };

    struct State {
        std::vector<Entry> entries;
        f32 frameRemainder = 0.0f;
        f32 lastAdvance = 0.0f;
        f64 heatFrames = 0.0;
    } m_state;
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
