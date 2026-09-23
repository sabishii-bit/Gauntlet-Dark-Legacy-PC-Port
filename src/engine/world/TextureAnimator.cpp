#include "engine/world/TextureAnimator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>

#include "engine/core/Log.h"

namespace gdl {

namespace {

/** The set holding a texture of this name, and the index there. */
struct Found {
    TextureSet* set = nullptr;
    std::uint32_t index = 0;
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

void TextureAnimator::bind(std::span<const TextureAnimationInfo> animations, TextureSet& textures,
                           RenderDevice& device, std::span<TextureSet* const> lenders) {
    clear();
    m_entryOfInfo.assign(animations.size(), -1);
    for (std::size_t i = 0; i < animations.size(); ++i) {
        const TextureAnimationInfo& animation = animations[i];
        if (animation.texture < 0 || animation.frames == 0 ||
            !(animation.cycles() || animation.scrolls())) {
            continue;
        }
        Entry entry;
        entry.slot = static_cast<std::uint32_t>(animation.texture);
        entry.period = std::abs(animation.frames);
        entry.rate = animation.rate;
        entry.counter = animation.start;
        entry.keyed = !animation.freeRunning();
        entry.offset = animation.offset;
        if (animation.scrolls()) {
            const float sign = animation.frames < 0 ? -1.0f : 1.0f;
            entry.direction = animation.source == TextureAnimationInfo::kScrollU ? Vec2{sign, 0.0f}
                                                                                 : Vec2{0.0f, sign};
            m_entryOfInfo[i] = static_cast<std::int32_t>(m_entries.size());
            m_entries.push_back(std::move(entry));
            continue;
        }
        std::optional<Found> first;
        if (animation.source >= 0) {
            first = Found{&textures, static_cast<std::uint32_t>(animation.source)};
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
        for (std::int32_t f = 0; f < entry.period; ++f) {
            const std::uint32_t index = found.index + static_cast<std::uint32_t>(f);
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
        entry.period = static_cast<std::int32_t>(entry.frames.size());
        entry.counter %= entry.period;
        m_entryOfInfo[i] = static_cast<std::int32_t>(m_entries.size());
        m_entries.push_back(std::move(entry));
    }
}

void TextureAnimator::clear() {
    m_entries.clear();
    m_entryOfInfo.clear();
    m_frame = 0;
}

float TextureAnimator::scrollAt(std::int32_t sinceStart, std::int32_t rate, std::int32_t frames) {
    return scrollStateAt(sinceStart, rate, frames).along;
}

/** The original's CalcTexScroll: the slide, and the stretch that is what it reaches less
 * the slide. */
ScrollState TextureAnimator::scrollStateAt(std::int32_t sinceStart, std::int32_t rate,
                                           std::int32_t frames) {
    const auto t = static_cast<float>(sinceStart);
    const auto lo = static_cast<float>(std::min(rate, frames));
    const auto hi = static_cast<float>(frames);
    ScrollState state;
    if (frames <= 0) {
        return state;
    }
    if (lo <= 0.0f) {
        state.along = static_cast<float>(sinceStart % frames) / hi;
        state.scale = 0.0f;
        return state;
    }
    const float scaled = hi / lo;
    float reach = 0.0f;
    if (t <= 0.0f) {
        state.along = 0.0f;
    } else if (t < lo) {
        // Eases in: the scroll runs from -2 * (hi / lo) up toward one over the first frames.
        state.along = (t / lo) * ((1.0f - scaled) + 2.0f * scaled) - 2.0f * scaled;
        reach = 1.0f;
    } else if (rate > frames) {
        state.along = 0.0f;
        reach = 1.0f;
    } else if (t < hi) {
        const float part = (t - lo) / (hi - lo);
        state.along = part * -(1.0f - scaled) + (1.0f - scaled);
        reach = part * (scaled - 1.0f) + 1.0f;
    } else if (t < hi + lo) {
        state.along = (t - hi) / lo;
        reach = scaled;
    } else {
        state.along = 1.0f;
        reach = scaled;
    }
    state.scale = reach - state.along;
    return state;
}

std::optional<TextureMotion> TextureAnimator::motionAt(std::int32_t info,
                                                       std::int32_t frame) const {
    if (info < 0 || static_cast<std::size_t>(info) >= m_entryOfInfo.size() ||
        m_entryOfInfo[static_cast<std::size_t>(info)] < 0) {
        return std::nullopt;
    }
    const Entry& entry =
        m_entries[static_cast<std::size_t>(m_entryOfInfo[static_cast<std::size_t>(info)])];
    TextureMotion motion;
    motion.slot = entry.slot;
    const std::int32_t since = frame - entry.offset;
    if (entry.frames.empty()) {
        const ScrollState scroll = scrollStateAt(since, entry.rate, entry.period);
        motion.offset = entry.direction * scroll.along;
        // The coordinate it slides is the one it stretches.
        motion.scale = Vec2{entry.direction.x != 0.0f ? scroll.scale : 1.0f,
                            entry.direction.y != 0.0f ? scroll.scale : 1.0f};
        return motion;
    }
    std::int32_t f = std::max(since, 0);
    if (entry.rate > 0) {
        f /= entry.rate;
    }
    f = std::min(f, entry.period - 1);
    motion.frame = entry.frames[static_cast<std::size_t>(f)];
    return motion;
}

TextureMotion TextureAnimator::motion(std::size_t index) const {
    const Entry& entry = m_entries[index];
    TextureMotion motion;
    motion.slot = entry.slot;
    if (entry.frames.empty()) {
        const float along =
            static_cast<float>(entry.counter % entry.period) / static_cast<float>(entry.period);
        motion.offset = entry.direction * along;
    } else {
        motion.frame = entry.frames[static_cast<std::size_t>(entry.counter)];
    }
    return motion;
}

void TextureAnimator::show(const Entry& entry, WorldScene& scene) {
    if (entry.frames.empty()) {
        const float along =
            static_cast<float>(entry.counter % entry.period) / static_cast<float>(entry.period);
        scene.setTextureOffset(entry.slot, entry.direction * along);
        return;
    }
    scene.setTextureFrame(entry.slot, entry.frames[static_cast<std::size_t>(entry.counter)]);
}

void TextureAnimator::step(std::uint32_t ticks) {
    for (std::uint32_t t = 0; t < ticks; ++t) {
        ++m_frame;
        for (Entry& entry : m_entries) {
            if (entry.keyed ||
                (entry.rate > 1 && m_frame % static_cast<std::uint32_t>(entry.rate) != 0)) {
                continue;
            }
            entry.counter = (entry.counter + 1) % entry.period;
        }
    }
}

void TextureAnimator::apply(WorldScene& scene) const {
    for (const Entry& entry : m_entries) {
        if (!entry.keyed) {
            show(entry, scene);
        }
    }
}

void TextureAnimator::step(WorldScene& scene, std::uint32_t ticks) {
    for (std::uint32_t t = 0; t < ticks; ++t) {
        ++m_frame;
        for (Entry& entry : m_entries) {
            if (entry.keyed ||
                (entry.rate > 1 && m_frame % static_cast<std::uint32_t>(entry.rate) != 0)) {
                continue;
            }
            entry.counter = (entry.counter + 1) % entry.period;
            show(entry, scene);
        }
    }
}

} // namespace gdl
