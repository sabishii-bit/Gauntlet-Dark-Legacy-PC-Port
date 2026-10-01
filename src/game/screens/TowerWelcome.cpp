#include "game/screens/TowerWelcome.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr std::string_view kWelcomeMessage = "WELCOMEMESSAGE";

} // namespace

void TowerWelcome::open(LevelWorld& world, bool welcome) {
    clear();
    m_pending = welcome;
    if (m_pending) {
        world.hideCrystals(); // Sumner reveals them once the scroll has gone
    }
}

void TowerWelcome::clear() {
    m_cutTicks = 0;
    m_pending = false;
    m_intro = Intro::None;
}

bool TowerWelcome::freshParty(std::span<const PartyMember> party) {
    return !party.empty() && std::ranges::all_of(party, [](const PartyMember& member) {
        return std::ranges::none_of(member.save.classes, [](const ClassProgress& progress) {
            return progress.experience > 0;
        });
    });
}

std::optional<s32> TowerWelcome::visitorOf(const LevelTriggers& triggers,
                                           std::span<const PlayerRuntime> players) {
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& spot = triggers.trigger(i);
        if (spot.id != kSumnerSpot) {
            continue;
        }
        for (const PlayerRuntime& runtime : players) {
            const PlayerActor& actor = runtime.actor;
            const Vec3 away = actor.position() - spot.spot;
            const f32 reach = spot.radius + actor.radius();
            if (away.x * away.x + away.z * away.z <= reach * reach &&
                std::abs(away.y) <= LevelTriggers::kReach) {
                return actor.player();
            }
        }
    }
    return std::nullopt;
}

void TowerWelcome::arrived(RenderDevice& device, LevelMessages& messages,
                           const StringTable* strings, const WorldLayout& layout,
                           SumnerFigure& sumner) {
    if (!m_pending) {
        return;
    }
    m_pending = false;
    if (messages.open(device, kWelcomeMessage, strings)) {
        m_intro = Intro::Scroll;
        return;
    }
    log::warn("Tower: no welcome scroll to show; on to the crystals");
    startCrystalCut(layout, sumner);
}

void TowerWelcome::scrollClosed(const WorldLayout& layout, SumnerFigure& sumner) {
    if (m_intro == Intro::Scroll) {
        startCrystalCut(layout, sumner);
    }
}

bool TowerWelcome::hold(s32 ticks) {
    if (m_intro != Intro::Crystal) {
        return false;
    }
    m_cutTicks -= ticks;
    if (m_cutTicks <= 0) {
        m_intro = Intro::Done;
    }
    return true;
}

std::optional<WorldCamera> TowerWelcome::camera() const {
    return m_intro == Intro::Crystal ? std::optional<WorldCamera>{m_cutCamera} : std::nullopt;
}

/** Sumner gestures at the crystals while the camera cuts to them from the level's marker. */
void TowerWelcome::startCrystalCut(const WorldLayout& layout, SumnerFigure& sumner) {
    sumner.gesture();
    const WorldLocator* marker = layout.findLocator(LocatorKind::TriggerCamera, kCrystalCamera);
    if (marker == nullptr) {
        log::warn("Tower: no crystal camera marker {}", kCrystalCamera);
        m_intro = Intro::Done;
        return;
    }
    m_cutCamera = WorldCamera{};
    m_cutCamera.position = marker->position;
    m_cutCamera.pitch = marker->rotation.x;
    m_cutCamera.yaw = marker->rotation.y;
    m_cutTicks = kCrystalTicks;
    m_intro = Intro::Crystal;
}

} // namespace gdl::game
