#include "game/screens/TowerPromotion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

#include "engine/core/Log.h"

#include "game/menu/ScrollBox.h"

namespace gdl::game {
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
        m_entries.push_back({i, level, caption(save, strings),
                             level == kMaxLevel ? "S_EXP99ALL"
                                                : std::format("S_EXP{}0{}", level / 10,
                                                              promotionClass(save.character))});
    }
}
void TowerPromotion::clear() {
    m_entries.clear();
    m_current = 0;
    m_elapsed = -120;
    m_spoken = m_awarded = m_gem = false;
    m_camera.reset();
    m_tree = nullptr;
    m_model.clear();
    m_player.stop();
    m_pose = TreePose{};
    m_textures = TextureAnimator{};
    m_frames = 0;
}
void TowerPromotion::bind(RenderDevice& device, ItemArchive& items, const WorldLayout& layout,
                          std::span<const PlayerRuntime> players) {
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
    m_transform =
        glm::rotate(glm::translate(Mat4{1}, nearest->position), nearest->rotation.y, Vec3{0, 1, 0});
    constexpr u32 kPromotionCameraBase = 240;
    if (const auto* camera =
            layout.findLocator(LocatorKind::TriggerCamera, kPromotionCameraBase + nearest->delay)) {
        m_camera = WorldCamera{};
        m_camera->position = camera->position;
        m_camera->pitch = camera->rotation.x;
        m_camera->yaw = camera->rotation.y;
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
    m_tree = &tree;
    m_model.setAppearance(true, Color::white(), false, true);
    m_textures.bind(items.trees.textureAnimations(), items.textures, device);
    m_player.start(tree.sequences[0], 0);
    animate(0);
}
usize TowerPromotion::shown() const {
    return static_cast<usize>(std::max(m_elapsed, 0) / 2);
}
TowerPromotion::Cue TowerPromotion::update(s32 ticks, bool voicePlaying) {
    Cue cue;
    if (!active()) {
        return cue;
    }
    m_elapsed += std::max(ticks, 0);
    if (!m_spoken && m_elapsed >= 0) {
        cue.voice = m_spoken = true;
    }
    if (!m_awarded && m_elapsed >= 240) {
        cue.award = m_awarded = true;
    }
    if (!m_gem && m_elapsed >= 270) {
        cue.gem = m_gem = true;
    }
    if (!cue.voice && !cue.award && !cue.gem && m_elapsed >= 360 &&
        shown() >= current()->caption.size() && !voicePlaying) {
        ++m_current;
        m_elapsed = 0;
        m_spoken = m_awarded = m_gem = false;
    }
    return cue;
}
void TowerPromotion::animate(f32 seconds) {
    if (m_tree == nullptr) {
        return;
    }
    m_player.advance(seconds, true);
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    const auto frame = static_cast<s32>(m_player.frame());
    m_model.setFrame(m_player.sequence(), frame);
    m_frames += seconds * AnimationPlayer::kDefaultRate;
    const auto elapsed = static_cast<u32>(std::floor(m_frames));
    m_frames -= static_cast<f32>(elapsed);
    m_textures.step(elapsed);
    m_textures.apply(m_model, *m_tree, m_player.sequence(), frame);
}
void TowerPromotion::draw(RenderDevice& device, const Mat4& clip,
                          const WorldLighting& lighting) const {
    if (active() && m_tree != nullptr) {
        const auto frame = m_camera.has_value() ? CameraFrame::of(*m_camera) : CameraFrame{};
        m_model.draw(device, clip, m_transform, lighting, m_pose.matrices(), &frame);
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
