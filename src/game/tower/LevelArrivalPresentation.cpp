#include "game/tower/LevelArrivalPresentation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {
namespace {
constexpr std::string_view kSpawnEffect = "STARTFX";
constexpr s32 kTitleY = 48;
constexpr s32 kTitleLift = 16;
constexpr f32 kTitleSlideStart = 0.025f;
constexpr f32 kTitleSlideRate = 0.025f;
constexpr f32 kTitleSlideEnd = 2.0f;
} // namespace

void LevelArrivalPresentation::appendState(NativeStateDigest& digest, const Simulation& state,
                                           const TextureAnimator::Clock& textures) const {
    digest.string("LevelArrivalPresentation.native.1");
    digest.resource(m_archive);
    m_textures.appendBindingDigest(digest);
    digest.fields<4>(state);
    digest.fields<3>(textures);
}

void LevelArrivalPresentation::appendSpawn(NativeStateDigest& digest, const SpawnState& state) {
    const auto& [position, tree, pose, player, previousFrame, previousGeneration,
                 presentationAdvanced] = state;
    digest.value(position);
    digest.resource(tree);
    digest.values(pose, player, previousFrame, previousGeneration, presentationAdvanced);
}

bool LevelArrivalPresentation::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim, m_textures.clock());
    digest.wide(m_spawns.size());
    for (const auto& spawn : m_spawns) {
        appendSpawn(digest, spawn);
    }
    return digest.good();
}

bool LevelArrivalPresentation::appendLocalDigest(NativeStateDigest& digest,
                                                 const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, spawns, textures, owner, binding] = saved;
    appendState(digest, state, textures);
    digest.wide(spawns.size());
    for (const auto& spawn : spawns) {
        appendSpawn(digest, spawn);
    }
    return digest.good();
}

void LevelArrivalPresentation::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
    saved.m_textures = m_textures.clock();
    saved.m_spawns.resize(m_spawns.size());
    for (usize i = 0; i < m_spawns.size(); ++i) {
        saved.m_spawns[i] = static_cast<const SpawnState&>(m_spawns[i]);
    }
}

bool LevelArrivalPresentation::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding &&
           saved.m_spawns.size() == m_spawns.size();
}

bool LevelArrivalPresentation::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_textures.restoreClock(saved.m_textures);
    for (usize i = 0; i < m_spawns.size(); ++i) {
        static_cast<SpawnState&>(m_spawns[i]) = saved.m_spawns[i];
    }
    return true;
}

void LevelArrivalPresentation::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_spawns.clear();
    m_archive = nullptr;
    m_textures.clear();
    m_sim.ticks = 0;
    m_sim.camera.stop();
    m_sim.titleSlide = 0.0f;
    m_sim.titleLanded = false;
}

void LevelArrivalPresentation::begin(RenderDevice& device, ItemArchive& weapons,
                                     std::span<const Vec3> positions,
                                     const std::optional<WorldCamera>& marker,
                                     StartCamera::Mode mode, std::optional<Vec3> cameraFocus) {
    clear();
    m_archive = &weapons;
    m_sim.ticks = kSpawnTicks;
    m_sim.titleSlide = kTitleSlideStart;
    if (marker.has_value()) {
        Vec3 centre{0.0f};
        for (const Vec3& position : positions) {
            centre += position;
        }
        if (!positions.empty()) {
            centre /= static_cast<f32>(positions.size());
        }
        m_sim.camera.start(*marker, cameraFocus.value_or(centre), mode);
    }
    const auto tree = weapons.loaded() ? weapons.trees.find(kSpawnEffect) : std::nullopt;
    if (!tree.has_value()) {
        log::warn("Tower: no {} in WEAPONS; the party appears without it", kSpawnEffect);
        return;
    }
    const TreeInfo& effect = weapons.trees.tree(*tree);
    for (const Vec3& position : positions) {
        Spawn spawn;
        spawn.position = position;
        spawn.tree = &effect;
        if (!spawn.model.bind(effect, weapons.models, weapons.textures, device)) {
            continue;
        }
        if (!effect.sequences.empty()) {
            spawn.player.start(effect.sequences[0], 0);
            spawn.pose.evaluate(effect, 0, 0.0f);
            spawn.model.setFrame(0, 0);
        } else {
            spawn.pose.rest(effect);
        }
        m_spawns.push_back(std::move(spawn));
    }
    m_textures.bind(weapons.trees.textureAnimations(), weapons.textures, device);
}

void LevelArrivalPresentation::capturePresentation() {
    m_textures.advance(0);
    for (Spawn& spawn : m_spawns) {
        spawn.presentationAdvanced = false;
    }
}

void LevelArrivalPresentation::animate(f32 seconds) {
    if (!active()) {
        return;
    }
    m_textures.advance(seconds);
    for (Spawn& spawn : m_spawns) {
        spawn.previousFrame = spawn.player.presentationFrame();
        spawn.previousGeneration = spawn.player.generation();
        spawn.presentationAdvanced = seconds > 0;
        if (spawn.player.playing() && !spawn.player.finished()) {
            spawn.player.advance(seconds, false);
            spawn.pose.evaluate(*spawn.tree, spawn.player.sequence(), spawn.player.frame());
            spawn.model.setFrame(spawn.player.sequence(), static_cast<s32>(spawn.player.frame()));
        }
        m_textures.apply(spawn.model, *spawn.tree, spawn.player.sequence(), spawn.player.frame());
    }
}

void LevelArrivalPresentation::advance(s32 ticks, bool skip, const Vec3& followPosition,
                                       const Vec3& followAttention) {
    if (!active()) {
        return;
    }
    m_sim.ticks = std::max(m_sim.ticks - ticks, 0);
    m_sim.camera.update(ticks, skip, followPosition, followAttention);
    const bool sliding = m_sim.titleSlide < kTitleSlideEnd;
    m_sim.titleSlide += kTitleSlideRate * static_cast<f32>(ticks);
    if (m_sim.camera.phase() != StartCamera::Phase::Hold || m_sim.titleSlide > kTitleSlideEnd) {
        m_sim.titleSlide = kTitleSlideEnd;
    }
    m_sim.titleLanded = m_sim.titleLanded || (sliding && m_sim.titleSlide == kTitleSlideEnd);
}

std::optional<LevelArrivalPresentation::EffectPresentation>
LevelArrivalPresentation::effectPresentation(usize index) const {
    if (m_sim.ticks <= 0 || index >= m_spawns.size()) {
        return std::nullopt;
    }
    const auto& spawn = m_spawns[index];
    // drawEffects samples this continuous cursor, not the snapped gameplay pose.
    return EffectPresentation{spawn.position,
                              m_archive,
                              spawn.tree,
                              spawn.player.sequence(),
                              spawn.player.presentationFrame(),
                              spawn.player.generation(),
                              static_cast<f32>(m_textures.frame()) +
                                  m_textures.presentationOffset(1).value_or(0)};
}

void LevelArrivalPresentation::drawEffects(RenderDevice& device, const Mat4& clip,
                                           const WorldLighting& lighting, f32 frameBlend) const {
    if (m_sim.ticks <= 0) {
        return;
    }
    for (const Spawn& spawn : m_spawns) {
        TreePose visualPose;
        const TreePose* pose = &spawn.pose;
        const f32 blend = frameBlend < 0 || spawn.presentationAdvanced ? frameBlend : 1.0f;
        f32 frame = spawn.player.frame();
        if (blend >= 0 && spawn.player.playing()) {
            frame = spawn.previousGeneration == spawn.player.generation()
                        ? portable::lerp(spawn.previousFrame, spawn.player.presentationFrame(),
                                         std::clamp(blend, 0.0f, 1.0f))
                        : spawn.player.presentationFrame();
            visualPose.evaluate(*spawn.tree, spawn.player.sequence(), frame,
                                TreePose::Sampling::Smooth);
            pose = &visualPose;
        }
        spawn.model.setPresentationFrame(spawn.player.sequence(), frame);
        m_textures.apply(spawn.model, *spawn.tree, spawn.player.sequence(), frame,
                         m_textures.presentationOffset(blend));
        spawn.model.draw(device, clip, glm::translate(Mat4{1.0f}, spawn.position), lighting,
                         pose->matrices());
    }
}

void LevelArrivalPresentation::drawTitle(Canvas& canvas, const TextPainter& text,
                                         std::string_view title, f32 width) const {
    drawTitleAt(canvas, text, title, width, titleScale());
}

void LevelArrivalPresentation::drawTitleAt(Canvas& canvas, const TextPainter& text,
                                           std::string_view title, f32 width, f32 scale) {
    if (!(scale > 0) || !std::isfinite(scale) || title.empty() || !text.ready()) {
        return;
    }
    TextStyle style;
    style.scale = std::min(scale, kTitleSlideEnd);
    const s32 y = kTitleY - static_cast<s32>(static_cast<f32>(kTitleLift) * style.scale);
    text.draw(canvas, -static_cast<s32>(width / 2.0f), y, title, style);
}

} // namespace gdl::game
