#include "engine/world/TextureAnimator.h"

#include <cstdlib>
#include <exception>
#include <optional>

#include "engine/core/Log.h"

namespace gdl {

namespace {

/** The set holding a texture of this name, and the index there. */
struct Found {
    TextureSet* set = nullptr;
    u32 index = 0;
};

std::optional<Found> findFrame(std::string_view name, TextureSet& textures,
                               std::span<TextureSet* const> lenders) {
    if (const auto index = textures.find(name); index.has_value()) {
        return Found{&textures, *index};
    }
    for (TextureSet* lender : lenders) {
        if (lender == nullptr) {
            continue;
        }
        if (const auto index = lender->find(name); index.has_value()) {
            return Found{lender, *index};
        }
    }
    return std::nullopt;
}

} // namespace

void TextureAnimator::bind(std::span<const TextureAnimationInfo> animations,
                           TextureSet& textures, RenderDevice& device,
                           std::span<TextureSet* const> lenders) {
    clear();
    for (const TextureAnimationInfo& animation : animations) {
        if (animation.texture < 0 || animation.frames == 0 ||
            !(animation.cycles() || animation.scrolls())) {
            continue;
        }
        Entry entry;
        entry.slot = static_cast<u32>(animation.texture);
        entry.period = std::abs(animation.frames);
        entry.rate = animation.rate;
        entry.counter = animation.start;
        if (animation.scrolls()) {
            const f32 sign = animation.frames < 0 ? -1.0f : 1.0f;
            entry.direction = animation.source == TextureAnimationInfo::kScrollU
                                  ? Vec2{sign, 0.0f}
                                  : Vec2{0.0f, sign};
            m_entries.push_back(std::move(entry));
            continue;
        }
        std::optional<Found> first;
        if (animation.source >= 0) {
            first = Found{&textures, static_cast<u32>(animation.source)};
        } else {
            first = findFrame(animation.frameName, textures, lenders);
        }
        if (!first.has_value()) {
            log::warn("Texture animation {}: its frames ({}) were not found", animation.name,
                      animation.frameName);
            continue;
        }
        const Found found = *first;
        if (found.index >= found.set->size()) {
            log::warn("Texture animation {}: its frames start past the set", animation.name);
            continue;
        }
        // The cycle ends early where the set ends or a frame cannot be read.
        for (s32 f = 0; f < entry.period; ++f) {
            const u32 index = found.index + static_cast<u32>(f);
            if (index >= found.set->size()) {
                break;
            }
            try {
                entry.frames.push_back(&found.set->texture(device, index));
            } catch (const std::exception& e) {
                log::warn("Texture animation {}: frame {}: {}", animation.name, f, e.what());
                break;
            }
        }
        if (entry.frames.empty()) {
            continue;
        }
        entry.period = static_cast<s32>(entry.frames.size());
        entry.counter %= entry.period;
        m_entries.push_back(std::move(entry));
    }
}

void TextureAnimator::clear() {
    m_entries.clear();
    m_frame = 0;
}

void TextureAnimator::show(const Entry& entry, WorldScene& scene) {
    if (entry.frames.empty()) {
        const f32 along = static_cast<f32>(entry.counter % entry.period) /
                          static_cast<f32>(entry.period);
        scene.setTextureOffset(entry.slot, entry.direction * along);
        return;
    }
    scene.setTextureFrame(entry.slot, entry.frames[static_cast<usize>(entry.counter)]);
}

void TextureAnimator::apply(WorldScene& scene) const {
    for (const Entry& entry : m_entries) {
        show(entry, scene);
    }
}

void TextureAnimator::step(WorldScene& scene, u32 ticks) {
    for (u32 t = 0; t < ticks; ++t) {
        ++m_frame;
        for (Entry& entry : m_entries) {
            if (entry.rate > 1 && m_frame % static_cast<u32>(entry.rate) != 0) {
                continue;
            }
            entry.counter = (entry.counter + 1) % entry.period;
            show(entry, scene);
        }
    }
}

} // namespace gdl
