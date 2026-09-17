#include "engine/world/ParticleField.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/world/WorldScene.h"

namespace gdl {

namespace {

/** The texture named, from the level's set or a lender's; null when nobody has it. */
const Texture* findTexture(std::string_view name, TextureSet& textures, RenderDevice& device,
                           std::span<TextureSet* const> lenders) {
    if (name.empty()) {
        return nullptr;
    }
    const auto take = [&](TextureSet& set) -> const Texture* {
        const auto index = set.find(name);
        if (!index.has_value()) {
            return nullptr;
        }
        try {
            return &set.texture(device, *index);
        } catch (const std::exception& e) {
            log::warn("Particle texture {}: {}", name, e.what());
            return nullptr;
        }
    };
    if (const Texture* found = take(textures); found != nullptr) {
        return found;
    }
    for (TextureSet* lender : lenders) {
        if (lender == nullptr) {
            continue;
        }
        if (const Texture* found = take(*lender); found != nullptr) {
            return found;
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
        const ParticleDescriptor descriptor = ParticleDescriptor::fromTemplate(*source);
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
        m_entries.push_back(std::move(entry));
    }
    // Emitters sharing a texture and state draw as one.
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        return std::less<const Texture*>{}(a.texture, b.texture);
    });
}

void ParticleField::clear() {
    m_entries.clear();
    m_frameRemainder = 0.0f;
}

usize ParticleField::start(const ParticleDescriptor& descriptor, const Mat4& node,
                           const Texture* texture, u32 seed) {
    Entry entry;
    entry.emitter.start(descriptor, node, seed);
    entry.texture = texture;
    entry.state.blend = descriptor.additive ? BlendMode::Additive : BlendMode::Alpha;
    entry.state.alphaTest = WorldScene::kAlphaTest;
    entry.state.depthWrite = descriptor.depthWrite;
    m_entries.push_back(std::move(entry));
    return m_entries.size() - 1;
}

void ParticleField::setNode(usize index, const Mat4& node) {
    if (index < m_entries.size()) {
        m_entries[index].emitter.setNode(node);
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
    m_frameRemainder += seconds * kFrameRate;
    const f32 whole = std::floor(m_frameRemainder);
    m_frameRemainder -= whole;
    const auto frames = static_cast<u32>(whole);
    if (frames == 0) {
        return;
    }
    for (Entry& entry : m_entries) {
        entry.emitter.step(frames);
    }
}

void ParticleField::draw(RenderDevice& device, const Mat4& clip, const Vec3& right,
                         const Vec3& up) const {
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
        if (!open || entry.texture != texture || !(entry.state == state)) {
            flush();
            texture = entry.texture;
            state = entry.state;
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            open = true;
        }
        entry.emitter.draw(m_batch, right, up);
    }
    flush();
}

} // namespace gdl
