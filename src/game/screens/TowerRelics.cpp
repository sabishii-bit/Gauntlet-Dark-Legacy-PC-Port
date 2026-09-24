#include "game/screens/TowerRelics.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

#include "engine/core/Log.h"

#include "game/menu/ScrollBox.h"

namespace gdl::game {
namespace {
constexpr u16 kRunes = 0x1fff;
constexpr u16 kWindowShards = 0x1fe;
constexpr s32 kLeadTicks = 120;
constexpr s32 kReadingTicks = 120;
constexpr f32 kPlacementHold = 2;
std::optional<WorldCamera> cameraAt(const WorldLayout& layout, u32 id) {
    if (const auto* marker = layout.findLocator(LocatorKind::TriggerCamera, id)) {
        WorldCamera camera;
        camera.position = marker->position;
        camera.yaw = marker->rotation.y;
        camera.pitch = marker->rotation.x;
        camera.roll = marker->rotation.z;
        return camera;
    }
    return std::nullopt;
}
} // namespace

std::string TowerRelics::Entry::tree() const {
    return std::format("{}{}", kind == Kind::Rune ? "RUNE" : "SHARD",
                       kind == Kind::Rune ? index + 1 : index);
}
std::string_view TowerRelics::Entry::anchor() const {
    if (kind == Kind::Shard) {
        return "L1WINDOWFRAME";
    }
    return index == 12 ? "L1RUNE13" : "L1RUNEPLACE";
}
u32 TowerRelics::Entry::camera() const {
    if (kind == Kind::Shard) {
        return 202U;
    }
    return index == 12 ? 203U : 201U;
}
std::string_view TowerRelics::Entry::voice() const {
    constexpr std::array<std::string_view, 9> kShardVoices{
        "",           "S_SHRD4TWN", "S_SHRD4MNT", "S_SHRD4CST", "S_SHRD4SKY",
        "S_SHRD4FOR", "S_SHRD4DES", "S_SHRD4ICE", "S_SHRD4DRM"};
    return kind == Kind::Rune ? "S_FNDRUNEYOU" : kShardVoices.at(static_cast<usize>(index));
}
void TowerRelics::clear() {
    m_entries.clear();
    m_captions.clear();
    m_current = 0;
    m_runes = m_shards = 0;
    m_ticks = -kLeadTicks;
    m_spoken = false;
    m_phase = Phase::Speech;
    m_placementLeft = 0;
    m_figures.clear();
    m_wizard.clear();
    m_speechCamera.reset();
    m_placementCamera.reset();
    m_device = nullptr;
    m_world = nullptr;
}
void TowerRelics::begin(std::span<const Relics> party, const MessageTable& strings) {
    clear();
    u16 runes = 0;
    u16 shards = 0;
    for (const auto& relics : party) {
        runes |= relics.runes;
        shards |= relics.shards;
        m_runes |= static_cast<u16>(relics.runes & ~relics.pendingRunes);
        m_shards |= static_cast<u16>(relics.shards & ~relics.pendingShards);
    }
    m_runes &= kRunes;
    m_shards &= kWindowShards;
    const auto append = [&](Entry entry) {
        m_entries.push_back(entry);
        std::string caption;
        const auto message = strings.find(entry.kind == Kind::Rune ? "NEWRUNES" : "NEWSHARDS");
        const auto page = entry.kind == Kind::Rune ? 0U : static_cast<usize>(entry.index);
        if (message && page < strings.message(*message).pages.size()) {
            caption = strings.message(*message).pages[page];
        }
        m_captions.push_back(std::move(caption));
    };
    // Glass first, then the rune pedestal; a party member's banked collection
    // already supplies the shared display, even if another member just found it.
    for (s32 i = 1; i <= 8; ++i) {
        const Entry entry{Kind::Shard, i};
        if ((shards & ~m_shards & entry.bit()) != 0) {
            append(entry);
        }
    }
    for (s32 i = 0; i < Relics::kRuneCount; ++i) {
        const Entry entry{Kind::Rune, i};
        if ((runes & ~m_runes & entry.bit()) != 0) {
            append(entry);
        }
    }
}
void TowerRelics::acknowledge(Relics& relics, const Entry& entry) {
    auto& mask = entry.kind == Kind::Rune ? relics.pendingRunes : relics.pendingShards;
    mask &= static_cast<u16>(~entry.bit());
}
void TowerRelics::bind(RenderDevice& device, LevelWorld& world, const Vec3& partyCentre) {
    m_device = &device;
    m_world = &world;
    for (s32 i = 0; i < Relics::kRuneCount; ++i) {
        if ((m_runes & Entry{Kind::Rune, i}.bit()) != 0) {
            place({Kind::Rune, i}, true);
        }
    }
    for (s32 i = 1; i <= 8; ++i) {
        if ((m_shards & Entry{Kind::Shard, i}.bit()) != 0) {
            place({Kind::Shard, i}, true);
        }
    }
    f32 nearest = std::numeric_limits<f32>::max();
    for (const auto& marker : world.layout().locators()) {
        const f32 distance = glm::dot(marker.position - partyCentre, marker.position - partyCentre);
        if (marker.kind == LocatorKind::Event && distance < nearest) {
            nearest = distance;
            m_wizardPosition = marker.position;
            m_wizardYaw = marker.rotation.y + kPi;
            m_wizardMarker = marker.delay;
        }
    }
    updateLights();
    prepareSpeech();
}
f32 TowerRelics::place(const Entry& entry, bool settled) {
    if (m_world == nullptr || m_device == nullptr) {
        return 0;
    }
    const auto& objects = m_world->layout().objects();
    for (usize i = 0; i < objects.size(); ++i) {
        if (objects[i].name != entry.anchor()) {
            continue;
        }
        EffectTrees::Setting setting;
        setting.persistent = true;
        setting.loop = false;
        setting.settled = settled;
        setting.emitParticles = !settled;
        const Vec3 position{m_world->scene().worldTransform(i)[3]};
        if (m_figures.startSet(*m_device, m_world->items(), entry.tree(), position, setting) == 0) {
            return 0;
        }
        const auto& effect = m_figures.effect(m_figures.count() - 1);
        return static_cast<f32>(effect.player.frameCount()) * effect.player.secondsPerFrame();
    }
    log::warn("Tower relic {} has no anchor {}", entry.tree(), entry.anchor());
    return 0;
}
void TowerRelics::prepareSpeech() {
    m_ticks = -kLeadTicks;
    m_spoken = false;
    m_phase = Phase::Speech;
    m_wizard.clear();
    if (!active() || m_world == nullptr || m_device == nullptr) {
        return;
    }
    // Three camera banks: promotion, window shard, runestone. Missing special
    // views fall back through the lower banks, just like the authored lookup.
    const std::array<u32, 3> bases{240, 220, 170};
    s32 bank = current()->kind == Kind::Rune ? 2 : 1;
    do {
        m_speechCamera =
            cameraAt(m_world->layout(), bases[static_cast<usize>(bank)] + m_wizardMarker);
    } while (!m_speechCamera && --bank >= 0);
    m_placementCamera = cameraAt(m_world->layout(), current()->camera());
    EffectTrees::Setting setting;
    setting.persistent = true;
    setting.yaw = m_wizardYaw;
    setting.unlit = true;
    setting.depthWrite = false;
    setting.additive = true;
    m_wizard.startSet(*m_device, m_world->items(), "WIZARD", m_wizardPosition, setting);
}
const std::optional<WorldCamera>& TowerRelics::camera() const {
    return m_phase == Phase::Placement ? m_placementCamera : m_speechCamera;
}
void TowerRelics::animate(f32 seconds) {
    m_figures.update(seconds);
    m_wizard.update(seconds);
}
TowerRelics::Cue TowerRelics::update(s32 ticks, f32 seconds, bool voicePlaying) {
    Cue cue;
    if (!active()) {
        return cue;
    }
    if (m_phase == Phase::Speech) {
        m_ticks += std::max(ticks, 0);
        if (m_ticks >= 0 && !m_spoken) {
            m_spoken = true;
            cue.voice = current()->voice();
        } else if (m_spoken && !voicePlaying &&
                   m_ticks >= static_cast<s32>(m_captions[m_current].size() * 2) + kReadingTicks) {
            m_phase = Phase::Placement;
            m_wizard.clear();
            m_placementLeft = place(*current(), false) + kPlacementHold;
            cue.placement = true;
        }
    } else {
        m_placementLeft -= std::max(seconds, 0.0f);
        if (m_placementLeft <= 0) {
            cue.completed = *current();
            auto& mask = current()->kind == Kind::Rune ? m_runes : m_shards;
            mask |= current()->bit();
            ++m_current;
            updateLights();
            prepareSpeech();
        }
    }
    return cue;
}
void TowerRelics::updateLights() {
    if (m_world == nullptr) {
        return;
    }
    const auto& objects = m_world->layout().objects();
    for (usize i = 0; i < objects.size(); ++i) {
        if (objects[i].name == "L1XPLOWERLIGHTR" || objects[i].name == "L1XPUPPERLIGHTR") {
            m_world->setObjectAlpha(i, m_shards == kWindowShards ? 1.0f : 0.0f);
        }
    }
}
void TowerRelics::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const WorldCamera& camera, bool ceremony) const {
    const auto frame = CameraFrame::of(camera);
    m_figures.draw(device, clip, lighting, &frame);
    if (ceremony && active() && m_phase == Phase::Speech) {
        m_wizard.draw(device, clip, lighting, &frame);
    }
}
void TowerRelics::drawCaption(Canvas& canvas, const TextPainter& text, f32 width,
                              f32 height) const {
    if (!active() || m_phase != Phase::Speech || !text.ready()) {
        return;
    }
    TextStyle style;
    style.scale = 0.667f;
    f32 y = height * 312.0f / 384.0f;
    auto remaining = static_cast<usize>(std::max(m_ticks, 0) / 2);
    for (const auto& line : ScrollBox::splitLines(m_captions[m_current])) {
        const auto visible = std::min(remaining, line.size());
        const auto x = text.leftEdge(-static_cast<s32>(width / 2), line, style.scale);
        text.draw(canvas, x, static_cast<s32>(y), line.substr(0, visible), style);
        if (remaining <= line.size()) {
            break;
        }
        remaining -= line.size() + 1;
        y += 32 * style.scale;
    }
}
} // namespace gdl::game
