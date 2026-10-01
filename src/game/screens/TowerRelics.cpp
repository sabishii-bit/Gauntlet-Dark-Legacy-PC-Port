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
constexpr s32 kPageHoldTicks = 60;
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
    if (kind == Kind::Followup) {
        return {};
    }
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
    if (kind == Kind::Followup) {
        return TowerCompletion::camera(followup());
    }
    if (kind == Kind::Shard) {
        return 202U;
    }
    return index == 12 ? 203U : 201U;
}
std::string_view TowerRelics::Entry::voice() const {
    if (kind == Kind::Followup) {
        return TowerCompletion::voice(followup());
    }
    constexpr std::array<std::string_view, 9> kShardVoices{
        "",           "S_SHRD4TWN", "S_SHRD4MNT", "S_SHRD4CST", "S_SHRD4SKY",
        "S_SHRD4FOR", "S_SHRD4DES", "S_SHRD4ICE", "S_SHRD4DRM"};
    return kind == Kind::Rune ? "S_FNDRUNEYOU" : kShardVoices.at(static_cast<usize>(index));
}
void TowerRelics::clear() {
    m_entries.clear();
    m_captions.clear();
    m_current = 0;
    m_page = 0;
    m_pendingCeremonies = 0;
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
    m_pendingCeremonies = TowerCompletion::pending(party);
    const auto append = [&](Entry entry) {
        m_entries.push_back(entry);
        std::string caption;
        const auto message = strings.find(entry.kind == Kind::Rune ? "NEWRUNES" : "NEWSHARDS");
        const auto page = entry.kind == Kind::Rune ? 0U : static_cast<usize>(entry.index);
        if (message && page < strings.message(*message).pages.size()) {
            caption = strings.message(*message).pages[page];
        }
        m_captions.push_back({std::move(caption)});
    };
    // Glass first, then the rune pedestal; a party member's banked collection
    // already supplies the shared display, even if another member just found it.
    for (s32 i = 1; i <= 8; ++i) {
        const Entry entry{Kind::Shard, i};
        if ((shards & ~m_shards & entry.bit()) != 0) {
            append(entry);
        }
    }
    appendFollowup(TowerCompletion::Kind::MoreShards, strings);
    appendFollowup(TowerCompletion::Kind::Window, strings);
    for (s32 i = 0; i < Relics::kRuneCount; ++i) {
        if (i == 12) {
            appendFollowup(TowerCompletion::Kind::TwelveWaiting, strings);
            appendFollowup(TowerCompletion::Kind::Underworld, strings);
        }
        const Entry entry{Kind::Rune, i};
        if ((runes & ~m_runes & entry.bit()) != 0) {
            append(entry);
        }
    }
    appendFollowup(TowerCompletion::Kind::Garm, strings);
    prepareEntry();
}
void TowerRelics::appendFollowup(TowerCompletion::Kind kind, const MessageTable& strings) {
    if ((m_pendingCeremonies & TowerCompletion::bit(kind)) == 0) {
        return;
    }
    m_entries.push_back({Kind::Followup, static_cast<s32>(kind)});
    std::vector<std::string> pages;
    if (const auto message = strings.find(TowerCompletion::message(kind))) {
        pages = strings.message(*message).pages;
    }
    if (pages.empty()) {
        pages.emplace_back();
    }
    m_captions.push_back(std::move(pages));
}
void TowerRelics::acknowledge(Relics& relics, const Entry& entry) {
    if (entry.kind == Kind::Followup) {
        relics.pendingCeremonies &= static_cast<u16>(~entry.bit());
        return;
    }
    auto& mask = entry.kind == Kind::Rune ? relics.pendingRunes : relics.pendingShards;
    mask &= static_cast<u16>(~entry.bit());
}
void TowerRelics::bind(RenderDevice& device, LevelWorld& world, const Vec3& partyCentre) {
    m_device = &device;
    m_world = &world;
    if (m_runes == kRunes &&
        (m_pendingCeremonies & TowerCompletion::bit(TowerCompletion::Kind::Garm)) == 0) {
        world.activateTrigger(255, true);
    }
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
    prepareEntry();
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
    m_page = 0;
    m_phase = Phase::Speech;
    m_wizard.clear();
    if (!active() || m_world == nullptr || m_device == nullptr) {
        return;
    }
    // Three camera banks: promotion, window shard, runestone. Missing special
    // views fall back through the lower banks, just like the authored lookup.
    const std::array<u32, 3> bases{240, 220, 170};
    const bool rune = current()->kind == Kind::Rune ||
                      (current()->kind == Kind::Followup &&
                       current()->followup() >= TowerCompletion::Kind::TwelveWaiting);
    s32 bank = rune ? 2 : 1;
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
void TowerRelics::prepareEntry() {
    prepareSpeech();
    if (!active() || current()->kind != Kind::Followup ||
        !TowerCompletion::reveals(current()->followup())) {
        return;
    }
    m_phase = Phase::Reveal;
    m_revealStarted = false;
    m_ticks = 0;
    m_wizard.clear();
    if (m_world != nullptr) {
        m_placementCamera = cameraAt(m_world->layout(), current()->camera());
        if (!m_placementCamera) {
            m_placementCamera =
                cameraAt(m_world->layout(),
                         current()->followup() == TowerCompletion::Kind::Window ? 202U : 203U);
        }
        if (!m_placementCamera) {
            m_placementCamera = cameraAt(m_world->layout(), 201);
        }
    }
}
const std::optional<WorldCamera>& TowerRelics::camera() const {
    return m_phase == Phase::Placement || m_phase == Phase::Reveal ? m_placementCamera
                                                                   : m_speechCamera;
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
    if (m_phase == Phase::Reveal) {
        if (!m_revealStarted) {
            m_revealStarted = true;
            if (m_world != nullptr && current()->followup() == TowerCompletion::Kind::Garm) {
                m_world->activateTrigger(255, false);
            }
        }
        m_ticks += std::max(ticks, 0);
        updateLights();
        if (m_ticks >= TowerCompletion::kRevealTicks) {
            prepareSpeech();
        }
        return cue;
    }
    if (m_phase == Phase::Speech) {
        m_ticks += std::max(ticks, 0);
        if (m_ticks >= 0 && !m_spoken) {
            m_spoken = true;
            cue.voice = current()->voice();
        } else if (m_spoken &&
                   m_ticks >= static_cast<s32>(caption().size() * 2) +
                                  (m_page + 1 < m_captions[m_current].size() ? kPageHoldTicks
                                                                             : kReadingTicks)) {
            if (m_page + 1 < m_captions[m_current].size()) {
                ++m_page;
                m_ticks = 0;
                return cue;
            }
            if (voicePlaying) {
                return cue;
            }
            if (current()->kind == Kind::Followup) {
                finish(cue);
                return cue;
            }
            m_phase = Phase::Placement;
            m_wizard.clear();
            m_placementLeft = place(*current(), false) + kPlacementHold;
            cue.placement = true;
            cue.sound = "S_RUNEFALL";
            if (current()->kind == Kind::Shard) {
                cue.sound =
                    (m_shards | current()->bit()) == kWindowShards ? "S_SHRDS127" : "S_SHRD8";
            }
        }
    } else {
        m_placementLeft -= std::max(seconds, 0.0f);
        if (m_placementLeft <= 0) {
            cue.sound = current()->kind == Kind::Rune ? "S_RUNEHIT" : "S_STNDGLASS";
            finish(cue);
        }
    }
    return cue;
}
void TowerRelics::finish(Cue& cue) {
    cue.completed = *current();
    if (current()->kind == Kind::Followup) {
        m_pendingCeremonies &= static_cast<u16>(~current()->bit());
    } else {
        auto& mask = current()->kind == Kind::Rune ? m_runes : m_shards;
        mask |= current()->bit();
    }
    ++m_current;
    prepareEntry();
    updateLights();
}
std::string_view TowerRelics::caption() const {
    return active() ? m_captions[m_current][m_page] : std::string_view{};
}
f32 TowerRelics::revealAlpha(TowerCompletion::Kind kind) const {
    if ((m_pendingCeremonies & TowerCompletion::bit(kind)) == 0) {
        return 1;
    }
    if (active() && current()->kind == Kind::Followup && current()->followup() == kind) {
        return m_phase == Phase::Reveal
                   ? std::clamp(static_cast<f32>(m_ticks) / TowerCompletion::kRevealTicks, 0.0f,
                                1.0f)
                   : 1.0f;
    }
    return 0;
}
void TowerRelics::updateLights() {
    if (m_world == nullptr) {
        return;
    }
    const auto& objects = m_world->layout().objects();
    for (usize i = 0; i < objects.size(); ++i) {
        if (objects[i].name == "L1XPLIGHTRAY01") {
            m_world->setObjectAlpha(
                i, m_shards == kWindowShards ? revealAlpha(TowerCompletion::Kind::Window) : 0.0f);
        }
    }
}
void TowerRelics::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const WorldCamera& camera) const {
    const auto frame = CameraFrame::of(camera);
    m_figures.draw(device, clip, lighting, &frame);
}
void TowerRelics::drawWizard(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                             const WorldCamera& camera) const {
    const auto frame = CameraFrame::of(camera);
    if (active() && m_phase == Phase::Speech) {
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
    for (const auto& line : ScrollBox::splitLines(caption())) {
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
