#include "engine/world/ParticleField.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>

#include "engine/assets/TextureBindings.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/render/HeatDistortion.h"
#include "engine/world/ThermalMaterial.h"
#include "engine/world/WorldScene.h"

namespace gdl {

void visitNativeState(NativeStateDigest& digest, const ParticleField& state) {
    const auto& [entries, remainder, advance, heat] = state.m_state;
    digest.values(remainder, advance, heat);
    digest.wide(entries.size());
    // bind() batches authored markers by texture address, which differs across
    // independently loaded peers. Canonicalize only those marker slots. Entries
    // made by start() retain their native indices (TreeParticles addresses them
    // through its node-to-emitter table), including in a mixed field.
    std::vector<const ParticleField::Entry*> authored;
    for (const auto& entry : entries) {
        if (entry.sourceObject >= 0) {
            authored.push_back(&entry);
        } else if (entry.sourceObject != -1) {
            digest.invalidate();
        }
    }
    std::ranges::sort(authored, {}, &ParticleField::Entry::sourceObject);
    for (usize index = 1; index < authored.size(); ++index) {
        if (authored[index - 1]->sourceObject == authored[index]->sourceObject) {
            digest.invalidate(); // A layout marker cannot identify two emitters.
        }
    }
    const auto append = [&](const auto& entry) {
        const auto& [emitter, sourceObject, worldObject, texture, draw, presentedTexture,
                     nextTexture, textureBlend, steppedTexture] = entry;
        digest.values(emitter, sourceObject, worldObject);
        digest.resource(texture);
        const auto& [blend, lightmap, masked, next, fraction, scale, offset, alpha, cull,
                     depthWrite, depthTest, mipmaps, clamp, cutout, lightmapRgb, coverage, smooth, darken,
                     colorScale, whiteBlend, fog] = draw;
        digest.value(blend);
        digest.resource(lightmap);
        digest.resource(masked);
        digest.resource(next);
        digest.values(fraction, scale, offset, alpha, cull, depthWrite, depthTest, mipmaps, clamp,
                      cutout, lightmapRgb, coverage, smooth, darken, colorScale, whiteBlend, fog);
        // Only these four per-draw samples and m_batch are excluded: draw()
        // recomputes them from native texture clocks and interpolation alpha.
        static_cast<void>(presentedTexture);
        static_cast<void>(nextTexture);
        static_cast<void>(textureBlend);
        static_cast<void>(steppedTexture);
    };
    usize marker = 0;
    for (const auto& entry : entries) {
        append(entry.sourceObject >= 0 ? *authored[marker++] : entry);
    }
}

bool ParticleField::equalLocal(const ParticleField& other) const {
    if (m_state.frameRemainder != other.m_state.frameRemainder ||
        m_state.lastAdvance != other.m_state.lastAdvance ||
        m_state.heatFrames != other.m_state.heatFrames ||
        m_state.entries.size() != other.m_state.entries.size()) {
        return false;
    }
    for (usize index = 0; index < m_state.entries.size(); ++index) {
        const auto& a = m_state.entries[index];
        const auto& b = other.m_state.entries[index];
        if (a.sourceObject != b.sourceObject || a.worldObject != b.worldObject ||
            a.texture != b.texture || a.state != b.state || !a.emitter.equalLocal(b.emitter)) {
            return false;
        }
    }
    return true;
}

bool ParticleField::Checkpoint::valid() const {
    return entries.size() <= 256 && validValues();
}

bool ParticleField::Checkpoint::validValues() const {
    return std::isfinite(frameRemainder) && frameRemainder >= 0 && frameRemainder < 1 &&
           std::isfinite(lastAdvance) && lastAdvance >= 0 && lastAdvance <= 450 &&
           std::isfinite(heatFrames) && heatFrames >= 0 && heatFrames <= 30.0 * 24 * 60 * 60 * 30 &&
           std::ranges::all_of(entries, [](const Entry& entry) {
               return entry.worldObject >= -1 && entry.worldObject <= 1'000'000 &&
                      entry.emitter.valid();
           });
}

ParticleField::Checkpoint ParticleField::checkpoint() const {
    Checkpoint state;
    state.frameRemainder = m_state.frameRemainder;
    state.lastAdvance = m_state.lastAdvance;
    state.heatFrames = m_state.heatFrames;
    state.entries.reserve(m_state.entries.size());
    for (const auto& entry : m_state.entries) {
        state.entries.push_back({entry.emitter.checkpoint(), entry.worldObject});
    }
    return state;
}

bool ParticleField::canRestore(const Checkpoint& state) const {
    if (!state.valid() || state.entries.size() != m_state.entries.size()) {
        return false;
    }
    for (usize i = 0; i < m_state.entries.size(); ++i) {
        if (state.entries[i].worldObject != m_state.entries[i].worldObject ||
            !m_state.entries[i].emitter.canRestore(state.entries[i].emitter)) {
            return false;
        }
    }
    return true;
}

bool ParticleField::restore(const Checkpoint& state) {
    if (!canRestore(state)) {
        return false;
    }
    auto entries = m_state.entries;
    for (usize i = 0; i < entries.size(); ++i) {
        if (!entries[i].emitter.restore(state.entries[i].emitter)) {
            return false;
        }
    }
    m_state.entries = std::move(entries);
    m_state.frameRemainder = state.frameRemainder;
    m_state.lastAdvance = state.lastAdvance;
    m_state.heatFrames = state.heatFrames;
    clearTextureBlends();
    m_batch.clear();
    return true;
}

namespace {

void submitHeat(RenderDevice& device, const ParticleEmitter& emitter, const Mat4& clip,
                const Vec3& right, const Vec3& up, f32 frameOffset, f32 seconds) {
    if (!ThermalMaterial::particle(emitter.descriptor().texture) ||
        !emitter.descriptor().depthTest) {
        return;
    }
    Vec3 center{0.0f};
    f32 weight = 0.0f;
    f32 width = 0.0f;
    for (Particle particle : emitter.particles()) {
        particle.age += frameOffset;
        if (particle.age < 0.0f) {
            continue;
        }
        const f32 alpha = static_cast<f32>(emitter.colorOf(particle).a) / 255.0f;
        center += emitter.positionOf(particle) * alpha;
        width += emitter.widthOf(particle) * std::abs(emitter.spriteScale()) * alpha;
        weight += alpha;
    }
    if (weight <= 0.01f) {
        return;
    }
    const f32 radius = width / weight;
    // Warm air rises a little above the live flame, not above the emitter's stale marker.
    center = center / weight + Vec3{0.0f, radius * 0.5f, 0.0f};
    device.addHeatSource(HeatSource::project(clip, center, right, up, radius, seconds));
}

/** The texture named, from the level's set or a lender's; null when nobody has it. */
const Texture* findTexture(std::string_view name, TextureSet& textures, RenderDevice& device,
                           std::span<TextureSet* const> lenders) {
    if (name.empty()) {
        return nullptr;
    }
    if (const auto binding = TextureBindings(textures, lenders).named(name)) {
        try {
            return &binding->set->texture(device, binding->index);
        } catch (const std::exception& e) {
            log::warn("Particle texture {}: {}", name, e.what());
        }
    }
    return nullptr;
}

} // namespace

void ParticleField::bind(const WorldLayout& layout, TextureSet& textures, RenderDevice& device,
                         std::span<TextureSet* const> lenders, u32 seed) {
    clear();
    const std::vector<WorldObject>& objects = layout.objects();
    for (usize i = 0; i < objects.size(); ++i) {
        const WorldObject& object = objects[i];
        if (!object.particles()) {
            continue;
        }
        const std::string name = normalizeAssetName(object.name);
        const usize tag = name.find(kTag);
        if (tag == std::string::npos || tag + kTag.size() >= name.size()) {
            log::warn("Particle marker {} names no template", object.name);
            continue;
        }
        const ParticleTemplate* source = layout.findParticleTemplate(name[tag + kTag.size()]);
        if (source == nullptr) {
            log::warn("Particle marker {}: the level has no template {}", object.name,
                      name[tag + kTag.size()]);
            continue;
        }
        Entry entry;
        entry.sourceObject = static_cast<s32>(i);
        if ((object.flags & WorldObject::kAnimated) != 0) {
            entry.worldObject = static_cast<s32>(i);
        }
        ParticleDescriptor descriptor = ParticleDescriptor::fromTemplate(*source);
        // Named world systems repeat their emission envelope until deactivated.
        descriptor.forever = true;
        entry.emitter.start(descriptor, glm::translate(Mat4{1.0f}, layout.worldPosition(i)),
                            seed + static_cast<u32>(i));
        entry.texture = findTexture(descriptor.texture, textures, device, lenders);
        if (entry.texture == nullptr) {
            log::warn("Particle marker {}: texture {} not found; drawn white", object.name,
                      descriptor.texture);
            entry.texture = &device.whiteTexture();
        }
        entry.state.blend = descriptor.additive ? BlendMode::Additive : BlendMode::Alpha;
        entry.state.alphaTest = WorldScene::kAlphaTest;
        entry.state.depthWrite = descriptor.depthWrite;
        entry.state.depthTest = descriptor.depthTest;
        m_state.entries.push_back(std::move(entry));
    }
    // Emitters sharing a texture and state draw as one.
    std::stable_sort(m_state.entries.begin(), m_state.entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return std::less<const Texture*>{}(a.texture, b.texture);
                     });
}

void ParticleField::clear() {
    m_state.heatFrames = 0.0;
    m_state.entries.clear();
    m_state.frameRemainder = 0.0f;
    m_state.lastAdvance = 0.0f;
}

usize ParticleField::start(const ParticleDescriptor& descriptor, const Mat4& node,
                           const Texture* texture, u32 seed) {
    Entry entry;
    entry.emitter.start(descriptor, node, seed);
    entry.texture = texture;
    entry.state.blend = descriptor.additive ? BlendMode::Additive : BlendMode::Alpha;
    entry.state.alphaTest = WorldScene::kAlphaTest;
    entry.state.depthWrite = descriptor.depthWrite;
    entry.state.depthTest = descriptor.depthTest;
    m_state.entries.push_back(std::move(entry));
    return m_state.entries.size() - 1;
}

void ParticleField::setNode(usize index, const Mat4& node) {
    if (index < m_state.entries.size()) {
        m_state.entries[index].emitter.setNode(node);
    }
}

void ParticleField::syncNodes(const WorldScene& scene) {
    for (Entry& entry : m_state.entries) {
        if (entry.worldObject >= 0) {
            entry.emitter.setNode(scene.worldTransform(static_cast<usize>(entry.worldObject)));
        }
    }
}

void ParticleField::setTexture(usize index, const Texture& texture) {
    if (index < m_state.entries.size()) {
        m_state.entries[index].texture = &texture;
        m_state.entries[index].presentedTexture = nullptr;
        m_state.entries[index].steppedTexture = false;
    }
}

void ParticleField::setTextureBlend(usize index, const Texture& current, const Texture* next,
                                    f32 blend, bool stepped) {
    if (index < m_state.entries.size()) {
        auto& entry = m_state.entries[index];
        entry.presentedTexture = &current;
        entry.nextTexture = next;
        entry.textureBlend = blend;
        entry.steppedTexture = stepped;
    }
}

void ParticleField::clearTextureBlends() {
    for (Entry& entry : m_state.entries) {
        entry.presentedTexture = nullptr;
        entry.nextTexture = nullptr;
        entry.textureBlend = 0.0f;
        entry.steppedTexture = false;
    }
}

void ParticleField::setEmitting(usize index, bool emitting) {
    if (index < m_state.entries.size()) {
        m_state.entries[index].emitter.setEmitting(emitting);
    }
}

void ParticleField::setSpriteScale(usize index, f32 scale) {
    if (index < m_state.entries.size()) {
        m_state.entries[index].emitter.setSpriteScale(scale);
    }
}

void ParticleField::stop(usize index) {
    if (index < m_state.entries.size()) {
        m_state.entries[index].emitter.finish();
    }
}

void ParticleField::prune() {
    std::erase_if(m_state.entries, [](const Entry& entry) { return !entry.emitter.active(); });
}

usize ParticleField::particleCount() const {
    usize count = 0;
    for (const Entry& entry : m_state.entries) {
        count += entry.emitter.particles().size();
    }
    return count;
}

void ParticleField::step(f32 seconds) {
    clearTextureBlends();
    m_state.lastAdvance = std::max(seconds, 0.0f) * kFrameRate;
    m_state.frameRemainder += m_state.lastAdvance;
    const f32 whole = std::floor(m_state.frameRemainder);
    m_state.frameRemainder -= whole;
    const auto frames = static_cast<u32>(whole);
    m_state.heatFrames += static_cast<f64>(frames);
    if (frames == 0) {
        return;
    }
    for (Entry& entry : m_state.entries) {
        entry.emitter.step(frames);
    }
}

void ParticleField::draw(RenderDevice& device, const Mat4& clip, const Vec3& right, const Vec3& up,
                         f32 presentationAlpha) const {
    const f32 frameOffset =
        presentationAlpha < 0.0f
            ? 0.0f
            : std::clamp(m_state.frameRemainder -
                             m_state.lastAdvance *
                                 (1.0f - std::clamp(presentationAlpha, 0.0f, 1.0f)),
                         -1.0f, 1.0f);
    const Texture* texture = nullptr;
    DrawState state;
    bool open = false;
    const auto flush = [&]() {
        if (open) {
            m_batch.end();
            if (!m_batch.empty()) {
                device.draw(m_batch, *texture, clip, state);
            }
            open = false;
        }
    };
    for (const Entry& entry : m_state.entries) {
        if (entry.emitter.particles().empty()) {
            continue;
        }
        submitHeat(device, entry.emitter, clip, right, up, frameOffset,
                   static_cast<f32>((m_state.heatFrames + frameOffset) / kFrameRate));
        const Texture* presentedTexture = entry.texture;
        DrawState presentedState = entry.state;
        if (entry.presentedTexture != nullptr &&
            (entry.steppedTexture ||
             (presentationAlpha >= 0.0f &&
              (entry.state.blend == BlendMode::Additive || !entry.state.depthWrite)))) {
            presentedTexture = entry.presentedTexture;
            presentedState.nextTexture = entry.steppedTexture ? nullptr : entry.nextTexture;
            presentedState.textureBlend = entry.steppedTexture ? 0.0f : entry.textureBlend;
        }
        if (!open || presentedTexture != texture || !(presentedState == state)) {
            flush();
            texture = presentedTexture;
            state = presentedState;
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            open = true;
        }
        entry.emitter.draw(m_batch, right, up, frameOffset);
    }
    flush();
}

} // namespace gdl
