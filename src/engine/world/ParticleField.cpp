#include "engine/world/ParticleField.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>

#include "engine/assets/TextureBindings.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/render/HeatDistortion.h"
#include "engine/world/WorldScene.h"

namespace gdl {

namespace {

// Explicit native thermal textures: tower/forest torches, mountain embers, dragon breath.
// This is an opt-in port effect, not an inference that every additive sprite is hot.
bool thermalTexture(std::string_view name) {
    return name == "P_TORCH" || name == "EMBER_SPARK2" || name == "DRAGONBREATH" ||
           name == "FBALLX";
}

void submitHeat(RenderDevice& device, const ParticleEmitter& emitter, const Mat4& clip,
                const Vec3& right, const Vec3& up, f32 frameOffset, f32 seconds) {
    if (!thermalTexture(emitter.descriptor().texture) || !emitter.descriptor().depthTest) {
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
        m_entries.push_back(std::move(entry));
    }
    // Emitters sharing a texture and state draw as one.
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        return std::less<const Texture*>{}(a.texture, b.texture);
    });
}

void ParticleField::clear() {
    m_heatFrames = 0.0;
    m_entries.clear();
    m_frameRemainder = 0.0f;
    m_lastAdvance = 0.0f;
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
    m_entries.push_back(std::move(entry));
    return m_entries.size() - 1;
}

void ParticleField::setNode(usize index, const Mat4& node) {
    if (index < m_entries.size()) {
        m_entries[index].emitter.setNode(node);
    }
}

void ParticleField::syncNodes(const WorldScene& scene) {
    for (Entry& entry : m_entries) {
        if (entry.worldObject >= 0) {
            entry.emitter.setNode(scene.worldTransform(static_cast<usize>(entry.worldObject)));
        }
    }
}

void ParticleField::setTexture(usize index, const Texture& texture) {
    if (index < m_entries.size()) {
        m_entries[index].texture = &texture;
        m_entries[index].presentedTexture = nullptr;
    }
}

void ParticleField::setTextureBlend(usize index, const Texture& current, const Texture* next,
                                    f32 blend) {
    if (index < m_entries.size()) {
        auto& entry = m_entries[index];
        entry.presentedTexture = &current;
        entry.nextTexture = next;
        entry.textureBlend = blend;
    }
}

void ParticleField::clearTextureBlends() {
    for (Entry& entry : m_entries) {
        entry.presentedTexture = nullptr;
        entry.nextTexture = nullptr;
        entry.textureBlend = 0.0f;
    }
}

void ParticleField::setEmitting(usize index, bool emitting) {
    if (index < m_entries.size()) {
        m_entries[index].emitter.setEmitting(emitting);
    }
}

void ParticleField::setSpriteScale(usize index, f32 scale) {
    if (index < m_entries.size()) {
        m_entries[index].emitter.setSpriteScale(scale);
    }
}

void ParticleField::stop(usize index) {
    if (index < m_entries.size()) {
        m_entries[index].emitter.finish();
    }
}

void ParticleField::prune() {
    std::erase_if(m_entries, [](const Entry& entry) { return !entry.emitter.active(); });
}

usize ParticleField::particleCount() const {
    usize count = 0;
    for (const Entry& entry : m_entries) {
        count += entry.emitter.particles().size();
    }
    return count;
}

void ParticleField::step(f32 seconds) {
    clearTextureBlends();
    m_lastAdvance = std::max(seconds, 0.0f) * kFrameRate;
    m_frameRemainder += m_lastAdvance;
    const f32 whole = std::floor(m_frameRemainder);
    m_frameRemainder -= whole;
    const auto frames = static_cast<u32>(whole);
    m_heatFrames += static_cast<f64>(frames);
    if (frames == 0) {
        return;
    }
    for (Entry& entry : m_entries) {
        entry.emitter.step(frames);
    }
}

void ParticleField::draw(RenderDevice& device, const Mat4& clip, const Vec3& right, const Vec3& up,
                         f32 presentationAlpha) const {
    const f32 frameOffset =
        presentationAlpha < 0.0f
            ? 0.0f
            : std::clamp(m_frameRemainder -
                             m_lastAdvance * (1.0f - std::clamp(presentationAlpha, 0.0f, 1.0f)),
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
    for (const Entry& entry : m_entries) {
        if (entry.emitter.particles().empty()) {
            continue;
        }
        submitHeat(device, entry.emitter, clip, right, up, frameOffset,
                   static_cast<f32>((m_heatFrames + frameOffset) / kFrameRate));
        const Texture* presentedTexture = entry.texture;
        DrawState presentedState = entry.state;
        if (presentationAlpha >= 0.0f && entry.presentedTexture != nullptr &&
            (entry.state.blend == BlendMode::Additive || !entry.state.depthWrite)) {
            presentedTexture = entry.presentedTexture;
            presentedState.nextTexture = entry.nextTexture;
            presentedState.textureBlend = entry.textureBlend;
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
