#include "engine/world/TextureAnimator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <optional>
#include <unordered_map>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreeParticles.h"

namespace gdl {

namespace {

/** The set holding a texture of this name, and the index there. */
struct Found {
    TextureSet* set = nullptr;
    u32 index = 0;
};

f32 fadeAlpha(s32 since, s32 duration, bool fadeOut) {
    const f32 fraction =
        duration > 0 ? std::clamp(static_cast<f32>(since) / static_cast<f32>(duration), 0.0f, 1.0f)
                     : 0.0f;
    // DoTexFadeSub passes truncated TRANSPARENCY to MBTreeSetAlpha, which stores
    // 255 minus that value. Invert after quantizing, not before (halfway is 128).
    const f32 transparency = fadeOut ? fraction : 1.0f - fraction;
    return static_cast<f32>(255 - static_cast<u8>(transparency * 255.0f)) / 255.0f;
}

std::optional<Found> findFrame(std::string_view name, TextureSet& textures,
                               std::span<TextureSet* const> lenders) {
    if (const auto index = textures.find(name);
        index && !textures.entry(*index).external() && !textures.entry(*index).noPicture) {
        return Found{&textures, *index};
    }
    for (TextureSet* lender : lenders) {
        if (lender == nullptr) {
            continue;
        }
        if (const auto index = lender->find(name);
            index && !lender->entry(*index).external() && !lender->entry(*index).noPicture) {
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
    for (usize i = 0; i < animations.size(); ++i) {
        const TextureAnimationInfo& animation = animations[i];
        const bool fade = animation.fades() && !animation.freeRunning();
        if (!fade && (animation.texture < 0 || animation.frames == 0 ||
                      !(animation.cycles() || animation.scrolls()))) {
            continue;
        }
        Entry entry;
        entry.slot = static_cast<u32>(animation.texture);
        entry.period = std::abs(animation.frames);
        entry.rate = animation.rate;
        entry.counter = animation.start;
        entry.keyed = !animation.freeRunning();
        entry.offset = animation.offset;
        if (fade) {
            entry.fade = animation.source == TextureAnimationInfo::kFadeOut ? -1 : 1;
            entry.period = animation.frames;
            m_entryOfInfo[i] = static_cast<s32>(m_entries.size());
            m_entries.push_back(std::move(entry));
            continue;
        }
        if (animation.scrolls()) {
            const f32 sign = animation.frames < 0 ? -1.0f : 1.0f;
            entry.direction = animation.source == TextureAnimationInfo::kScrollU ? Vec2{sign, 0.0f}
                                                                                 : Vec2{0.0f, sign};
            m_entryOfInfo[i] = static_cast<s32>(m_entries.size());
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
        m_entryOfInfo[i] = static_cast<s32>(m_entries.size());
        m_entries.push_back(std::move(entry));
    }
}

void TextureAnimator::clear() {
    m_entries.clear();
    m_entryOfInfo.clear();
    m_frame = 0;
    m_remainder = 0.0f;
    m_advance = 0.0f;
}

f32 TextureAnimator::scrollAt(s32 sinceStart, s32 rate, s32 frames) {
    return scrollStateAt(sinceStart, rate, frames).along;
}

/** The original's CalcTexScroll: the slide, and the stretch that is what it reaches less
 * the slide. */
ScrollState TextureAnimator::scrollStateAt(f32 sinceStart, s32 rate, s32 frames) {
    const f32 t = sinceStart;
    const auto lo = static_cast<f32>(std::min(rate, frames));
    const auto hi = static_cast<f32>(frames);
    ScrollState state;
    if (frames <= 0) {
        return state;
    }
    if (lo <= 0.0f) {
        state.along = std::fmod(sinceStart, hi) / hi;
        state.scale = 0.0f;
        return state;
    }
    const f32 scaled = hi / lo;
    f32 reach = 0.0f;
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
        const f32 part = (t - lo) / (hi - lo);
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

std::optional<TextureMotion> TextureAnimator::motionAt(s32 info, f32 frame) const {
    if (info < 0 || static_cast<usize>(info) >= m_entryOfInfo.size() ||
        m_entryOfInfo[static_cast<usize>(info)] < 0) {
        return std::nullopt;
    }
    const Entry& entry = m_entries[static_cast<usize>(m_entryOfInfo[static_cast<usize>(info)])];
    TextureMotion motion;
    motion.slot = entry.slot;
    const f32 since = frame - static_cast<f32>(entry.offset);
    if (entry.fade != 0) {
        motion.alpha = fadeAlpha(static_cast<s32>(since), entry.period, entry.fade < 0);
        return motion;
    }
    if (entry.frames.empty()) {
        const ScrollState scroll = scrollStateAt(since, entry.rate, entry.period);
        motion.offset = entry.direction * scroll.along;
        // The coordinate it slides is the one it stretches.
        motion.scale = Vec2{entry.direction.x != 0.0f ? scroll.scale : 1.0f,
                            entry.direction.y != 0.0f ? scroll.scale : 1.0f};
        return motion;
    }
    const f32 sample = std::min(std::max(since, 0.0f) / static_cast<f32>(std::max(entry.rate, 1)),
                                static_cast<f32>(entry.period - 1));
    const auto index = static_cast<usize>(sample);
    motion.frame = entry.frames[index];
    motion.nextFrame = entry.frames[std::min(index + 1, entry.frames.size() - 1)];
    motion.frameBlend = sample - std::floor(sample);
    return motion;
}

TextureMotion TextureAnimator::motion(usize index, std::optional<f32> frameOffset) const {
    const Entry& entry = m_entries[index];
    TextureMotion motion;
    motion.slot = entry.slot;
    if (entry.fade != 0) {
        motion.alpha = fadeAlpha(entry.counter - entry.offset, entry.period, entry.fade < 0);
    } else if (frameOffset.has_value()) {
        const auto rate = static_cast<u32>(std::max(entry.rate, 1));
        const f32 sample =
            static_cast<f32>(entry.counter) +
            (static_cast<f32>(m_frame % rate) + *frameOffset) / static_cast<f32>(rate);
        const f32 period = static_cast<f32>(entry.period);
        const f32 wrapped = sample - std::floor(sample / period) * period;
        if (entry.frames.empty()) {
            motion.offset = entry.direction * (wrapped / period);
        } else {
            const auto current = static_cast<usize>(wrapped);
            motion.frame = entry.frames[current];
            motion.nextFrame = entry.frames[(current + 1) % entry.frames.size()];
            motion.frameBlend = wrapped - std::floor(wrapped);
        }
    } else if (entry.frames.empty()) {
        const f32 along =
            static_cast<f32>(entry.counter % entry.period) / static_cast<f32>(entry.period);
        motion.offset = entry.direction * along;
    } else {
        motion.frame = entry.frames[static_cast<usize>(entry.counter)];
    }
    return motion;
}

void TextureAnimator::advance(f32 seconds) {
    constexpr f32 kFrameRate = 30.0f;
    m_advance = std::max(seconds, 0.0f) * kFrameRate;
    m_remainder += m_advance;
    const auto whole = static_cast<u32>(m_remainder);
    m_remainder -= static_cast<f32>(whole);
    step(whole);
}

std::optional<f32> TextureAnimator::presentationOffset(f32 alpha) const {
    if (alpha < 0) {
        return std::nullopt;
    }
    return m_remainder - m_advance * (1.0f - std::clamp(alpha, 0.0f, 1.0f));
}

void TextureAnimator::show(const Entry& entry, WorldScene& scene) {
    if (entry.frames.empty()) {
        const f32 along =
            static_cast<f32>(entry.counter % entry.period) / static_cast<f32>(entry.period);
        scene.setTextureOffset(entry.slot, entry.direction * along);
        return;
    }
    scene.setTextureFrame(entry.slot, entry.frames[static_cast<usize>(entry.counter)]);
}

void TextureAnimator::step(u32 ticks) {
    for (u32 t = 0; t < ticks; ++t) {
        ++m_frame;
        for (Entry& entry : m_entries) {
            if (entry.keyed || (entry.rate > 1 && m_frame % static_cast<u32>(entry.rate) != 0)) {
                continue;
            }
            entry.counter = (entry.counter + 1) % entry.period;
        }
    }
}

void TextureAnimator::apply(WorldScene& scene) const {
    // Separate U and V records may address the same texture (Temple rain).
    // Compose this frame's records, not the previous frame's offset.
    struct Scroll {
        Vec2 offset{0};
        Vec2 phase{0};
        Vec2 velocity{0};
    };
    std::unordered_map<u32, Scroll> offsets;
    for (const Entry& entry : m_entries) {
        if (!entry.keyed) {
            if (entry.frames.empty() && entry.fade == 0) {
                auto& scroll = offsets[entry.slot];
                const f32 along =
                    static_cast<f32>(entry.counter % entry.period) / static_cast<f32>(entry.period);
                const auto rate = static_cast<u32>(std::max(entry.rate, 1));
                const f32 speed = 1.0f / (static_cast<f32>(entry.period) * static_cast<f32>(rate));
                const f32 phase = static_cast<f32>(m_frame % rate) * speed;
                if (entry.direction.x != 0) {
                    scroll.offset.x = entry.direction.x * along;
                    scroll.phase.x = entry.direction.x * phase;
                    scroll.velocity.x = entry.direction.x * speed;
                }
                if (entry.direction.y != 0) {
                    scroll.offset.y = entry.direction.y * along;
                    scroll.phase.y = entry.direction.y * phase;
                    scroll.velocity.y = entry.direction.y * speed;
                }
            } else {
                show(entry, scene);
                if (!entry.frames.empty()) {
                    const auto rate = static_cast<u32>(std::max(entry.rate, 1));
                    scene.setTextureCycle(entry.slot, entry.frames,
                                          static_cast<f32>(entry.counter) +
                                              static_cast<f32>(m_frame % rate) /
                                                  static_cast<f32>(rate),
                                          1.0f / static_cast<f32>(rate));
                }
            }
        }
    }
    for (const auto& [slot, scroll] : offsets) {
        scene.setTextureScroll(slot, scroll.offset, scroll.phase, scroll.velocity);
    }
}

void TextureAnimator::apply(TreeModel& model, const TreeInfo& tree, u32 sequence, f32 frame,
                            std::optional<f32> frameOffset) const {
    model.resetTextures();
    const auto show = [&](const TextureMotion& moved) {
        if (moved.alpha.has_value()) {
            model.setNodeAlpha(0, *moved.alpha);
        } else if (moved.frame != nullptr) {
            model.setTextureFrame(moved.slot, moved.frame,
                                  frameOffset.has_value() ? moved.nextFrame : nullptr,
                                  frameOffset.has_value() ? moved.frameBlend : 0.0f);
        } else {
            model.setTextureOffset(moved.slot, moved.offset, moved.scale);
        }
    };
    for (usize i = 0; i < size(); ++i) {
        if (!keyed(i)) {
            const auto moved = motion(i, frameOffset);
            if (moved.frame == nullptr && !moved.alpha) {
                Vec2 offset = model.textureOffset(moved.slot);
                const auto& direction = m_entries[i].direction;
                if (direction.x != 0) {
                    offset.x = moved.offset.x;
                }
                if (direction.y != 0) {
                    offset.y = moved.offset.y;
                }
                model.setTextureOffset(moved.slot, offset, moved.scale);
            } else {
                show(moved);
            }
        }
    }
    if (sequence >= tree.sequences.size()) {
        return;
    }
    const TreeSequenceInfo& selected = tree.sequences[sequence];
    frame = (selected.flags & 1U) != 0 && selected.frames > 0
                ? static_cast<f32>(selected.frames - 1) - frame
                : frame;
    for (s32 i = 0; i < selected.textureAnimationCount; ++i) {
        if (const auto moved = motionAt(selected.textureAnimationStart + i, frame)) {
            show(*moved);
        }
    }
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        if (const auto moved = motionAt(tree.nodes[i].textureAnimation, frame)) {
            if (moved->alpha.has_value()) {
                model.setNodeAlpha(i, *moved->alpha);
            } else if (moved->frame != nullptr) {
                model.setNodeTextureFrame(i, moved->slot, moved->frame,
                                          frameOffset.has_value() ? moved->nextFrame : nullptr,
                                          frameOffset.has_value() ? moved->frameBlend : 0.0f);
            } else {
                model.setNodeTextureOffset(i, moved->offset, moved->scale);
            }
        }
    }
}

void TextureAnimator::step(WorldScene& scene, u32 ticks) {
    step(ticks);
    apply(scene);
}

void TextureAnimator::apply(TreeParticles& particles, const TreeInfo& tree, u32 sequence, f32 frame,
                            std::optional<f32> frameOffset) const {
    particles.clearTextureBlends();
    const auto show = [&](const TextureMotion& moved) {
        if (moved.frame != nullptr) {
            if (frameOffset) {
                particles.setTextureBlend(moved.slot, *moved.frame, moved.nextFrame,
                                          moved.frameBlend);
            } else {
                particles.setTextureFrame(moved.slot, *moved.frame);
            }
        }
    };
    for (usize i = 0; i < size(); ++i) {
        if (!keyed(i)) {
            show(motion(i, frameOffset));
        }
    }
    if (sequence >= tree.sequences.size()) {
        return;
    }
    const auto& selected = tree.sequences[sequence];
    frame = (selected.flags & 1U) != 0 && selected.frames > 0
                ? static_cast<f32>(selected.frames - 1) - frame
                : frame;
    for (s32 i = 0; i < selected.textureAnimationCount; ++i) {
        if (const auto moved = motionAt(selected.textureAnimationStart + i, frame)) {
            show(*moved);
        }
    }
    for (const auto& node : tree.nodes) {
        if (const auto moved = motionAt(node.textureAnimation, frame)) {
            show(*moved);
        }
    }
}

} // namespace gdl
