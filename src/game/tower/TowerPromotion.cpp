#include "game/tower/TowerPromotion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

#include "engine/core/Log.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/widgets/ScrollBox.h"

namespace gdl::game {
void TowerPromotion::appendState(NativeStateDigest& digest, const Simulation& state,
                                 const TextureAnimator::Clock& clock) const {
    const auto& [entries, current, elapsed, spoken, awarded, gem, camera, transform, tree, pose,
                 player, previousFrame, previousGeneration, advanced] = state;
    digest.string("TowerPromotion.native.1");
    digest.values(entries, current, elapsed, spoken, awarded, gem, camera, transform);
    digest.resource(tree);
    digest.values(pose, player, previousFrame, previousGeneration, advanced);
    m_textures.appendBindingDigest(digest);
    digest.fields<3>(clock);
}

bool TowerPromotion::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim, m_textures.clock());
    return digest.good();
}

bool TowerPromotion::appendLocalDigest(NativeStateDigest& digest,
                                       const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, clock, owner, binding] = saved;
    appendState(digest, state, clock);
    return digest.good();
}

void TowerPromotion::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
    saved.m_textureClock = m_textures.clock();
}

bool TowerPromotion::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding;
}

bool TowerPromotion::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_textures.restoreClock(saved.m_textureClock);
    return true;
}

namespace {
// The model bank calls the dwarf DWF; the title and promotion banks use DWA.
std::string_view promotionClass(s32 character) {
    const auto code = classCode(character);
    return code == "DWF" ? "DWA" : code;
}

std::string line(const MessageTable& strings, std::string_view name, usize index = 0) {
    const auto found = strings.find(name);
    if (!found.has_value() || index >= strings.message(*found).pages.size()) {
        return {};
    }
    return strings.message(*found).pages[index];
}
} // namespace
std::string TowerPromotion::caption(const CharacterSave& save, const MessageTable& strings) {
    const s32 level = experienceLevel(save.experience());
    const std::string rank =
        level == kMaxLevel ? line(strings, "LEGEND")
                           : line(strings, std::format("{}_RANK", promotionClass(save.character)),
                                  static_cast<usize>(level / 20));
    std::string result = line(strings, "NEWLEVEL");
    const std::array values{line(strings, "PLAYER_COLOR_LC", static_cast<usize>(save.color)),
                            line(strings, "PLAYER_CLASS_LC", static_cast<usize>(save.character)),
                            std::to_string(level), rank};
    usize from = 0;
    for (const auto& value : values) {
        const auto at = result.find('%', from);
        if (at == std::string::npos || at + 1 >= result.size()) {
            break;
        }
        result.replace(at, 2, value);
        from = at + value.size();
    }
    return result;
}
void TowerPromotion::begin(std::span<const PlayerRuntime> players, const MessageTable& strings) {
    clear();
    for (usize i = 0; i < players.size(); ++i) {
        const auto& save = players[i].actor.save();
        if (players[i].life != PlayerLife::Standing || classCode(save.character) == "SUM" ||
            !save.progress().promotionPending()) {
            continue;
        }
        const s32 level = experienceLevel(save.experience());
        m_sim.entries.push_back({i, level, caption(save, strings),
                                 level == kMaxLevel ? "S_EXP99ALL"
                                                    : std::format("S_EXP{}0{}", level / 10,
                                                                  promotionClass(save.character))});
    }
}
void TowerPromotion::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_sim.entries.clear();
    m_sim.current = 0;
    m_sim.elapsed = -120;
    m_sim.spoken = m_sim.awarded = m_sim.gem = false;
    m_sim.camera.reset();
    m_sim.tree = nullptr;
    m_model.clear();
    m_sim.player.stop();
    m_sim.pose = TreePose{};
    m_textures = TextureAnimator{};
    m_sim.presentationAdvanced = false;
}
void TowerPromotion::bind(RenderDevice& device, ItemArchive& items, const WorldLayout& layout,
                          std::span<const PlayerRuntime> players) {
    m_localBinding = std::make_shared<const u8>(0);
    if (!active() || players.empty()) {
        return;
    }
    Vec3 centre{0};
    for (const auto& player : players) {
        centre += player.actor.position();
    }
    centre /= static_cast<f32>(players.size());
    const WorldLocator* nearest = nullptr;
    f32 distance = std::numeric_limits<f32>::max();
    for (const auto& marker : layout.locators()) {
        const f32 squared = glm::dot(marker.position - centre, marker.position - centre);
        if (marker.kind == LocatorKind::Event && squared < distance) {
            nearest = &marker;
            distance = squared;
        }
    }
    if (nearest == nullptr) {
        return;
    }
    // Event headings, like Sumner's lookout and player starts, point the way
    // the figure came from. Turn back toward the party, not away from it.
    m_sim.transform = portable::rotate(glm::translate(Mat4{1}, nearest->position),
                                       nearest->rotation.y + kPi, Vec3{0, 1, 0});
    constexpr u32 kPromotionCameraBase = 240;
    if (const auto* camera =
            layout.findLocator(LocatorKind::TriggerCamera, kPromotionCameraBase + nearest->delay)) {
        m_sim.camera = WorldCamera{};
        m_sim.camera->position = camera->position;
        m_sim.camera->pitch = camera->rotation.x;
        m_sim.camera->yaw = camera->rotation.y;
    }
    const auto index = items.trees.find("WIZARD");
    if (!index.has_value()) {
        return;
    }
    const auto& tree = items.trees.tree(*index);
    if (const auto body = tree.findNode("OANIM");
        body.has_value() && tree.nodes[*body].objectFrames.empty()) {
        log::warn("Tower wizard has no animated body track; refresh assets with "
                  "gdlunpack <asset-root> <unpacked-root> --only LEVELL");
    }
    if (tree.sequences.empty() || !m_model.bind(tree, items.models, items.textures, device)) {
        return;
    }
    m_sim.tree = &tree;
    m_model.setAppearance(true, Color::white(), false, true);
    m_textures.bind(items.trees.textureAnimations(), items.textures, device);
    m_sim.player.start(tree.sequences[0], 0);
    animate(0);
}
usize TowerPromotion::shown() const {
    return static_cast<usize>(std::max(m_sim.elapsed, 0) / 2);
}
TowerPromotion::Cue TowerPromotion::update(s32 ticks, bool voicePlaying) {
    Cue cue;
    if (!active()) {
        return cue;
    }
    m_sim.elapsed += std::max(ticks, 0);
    if (!m_sim.spoken && m_sim.elapsed >= 0) {
        cue.voice = m_sim.spoken = true;
    }
    if (!m_sim.awarded && m_sim.elapsed >= 240) {
        cue.award = m_sim.awarded = true;
    }
    if (!m_sim.gem && m_sim.elapsed >= 270) {
        cue.gem = m_sim.gem = true;
    }
    if (!cue.voice && !cue.award && !cue.gem && m_sim.elapsed >= 360 &&
        shown() >= current()->caption.size() && !voicePlaying) {
        ++m_sim.current;
        m_sim.elapsed = 0;
        m_sim.spoken = m_sim.awarded = m_sim.gem = false;
    }
    return cue;
}
void TowerPromotion::animate(f32 seconds) {
    if (m_sim.tree == nullptr) {
        return;
    }
    m_sim.previousFrame = m_sim.player.presentationFrame();
    m_sim.previousGeneration = m_sim.player.generation();
    m_sim.presentationAdvanced = seconds > 0;
    m_sim.player.advance(seconds, true);
    m_sim.pose.evaluate(*m_sim.tree, m_sim.player.sequence(), m_sim.player.frame());
    const auto frame = static_cast<s32>(m_sim.player.frame());
    m_model.setFrame(m_sim.player.sequence(), frame);
    m_textures.advance(seconds);
    m_textures.apply(m_model, *m_sim.tree, m_sim.player.sequence(), frame);
}
void TowerPromotion::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          f32 renderAlpha) const {
    if (active() && m_sim.tree != nullptr) {
        const f32 blend = renderAlpha < 0 || m_sim.presentationAdvanced ? renderAlpha : 1.0f;
        f32 visualFrame = m_sim.player.frame();
        TreePose visualPose;
        const TreePose* pose = &m_sim.pose;
        if (blend >= 0) {
            visualFrame =
                m_sim.previousGeneration == m_sim.player.generation()
                    ? portable::lerp(m_sim.previousFrame, m_sim.player.presentationFrame(),
                                     std::clamp(blend, 0.0f, 1.0f))
                    : m_sim.player.presentationFrame();
            visualPose.evaluate(*m_sim.tree, m_sim.player.sequence(), visualFrame, false, true);
            pose = &visualPose;
        }
        m_model.setPresentationFrame(m_sim.player.sequence(), visualFrame);
        m_textures.apply(m_model, *m_sim.tree, m_sim.player.sequence(), visualFrame,
                         m_textures.presentationOffset(blend));
        const auto frame =
            m_sim.camera.has_value() ? CameraFrame::of(*m_sim.camera) : CameraFrame{};
        m_model.draw(device, clip, m_sim.transform, lighting, pose->matrices(), &frame);
    }
}
void TowerPromotion::drawCaption(Canvas& canvas, const TextPainter& text, f32 width,
                                 f32 height) const {
    if (!active() || !text.ready()) {
        return;
    }
    const auto lines = ScrollBox::splitLines(current()->caption);
    TextStyle style;
    // CaptionTextSub starts at y=312 in the 512x384 canvas, below the picture.
    // Centre the complete line so its already-visible letters do not wander while typing.
    constexpr f32 kCaptionScale = 0.667f;
    constexpr f32 kReferenceHeight = 384;
    constexpr f32 kCaptionY = 312;
    constexpr f32 kFontLineHeight = 32;
    style.scale = kCaptionScale;
    f32 y = height * kCaptionY / kReferenceHeight;
    usize remaining = shown();
    for (const auto& value : lines) {
        const auto visible = std::min(remaining, value.size());
        const s32 x = text.leftEdge(-static_cast<s32>(width / 2), value, style.scale);
        text.draw(canvas, x, static_cast<s32>(y), value.substr(0, visible), style);
        if (remaining <= value.size()) {
            break;
        }
        remaining -= value.size() + 1;
        y += kFontLineHeight * style.scale;
    }
}
} // namespace gdl::game
