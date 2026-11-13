#include "game/tower/TowerWelcome.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/level/TowerAccess.h"

namespace gdl::game {

namespace {

constexpr std::string_view kWelcomeMessage = "WELCOMEMESSAGE";
constexpr std::string_view kGarmMessage = "GARMMESSAGE"; ///< 0x80113EEC
/** The level whose beating brings GarmMessage: the Underworld's second, its throne (do_players
 * compares 0x803448AC with 6 and 0x803448A8 with 1, 0x80076B60-0x80076B74). */
constexpr LevelBeaten kUnderworldThrone{TowerAccess::kUnderworldWorld, 1};

} // namespace

void TowerWelcome::open(LevelWorld& world, bool welcome, std::optional<LevelBeaten> beaten) {
    clear();
    m_due = welcome ? Due::Welcome : beaten == kUnderworldThrone ? Due::Garm : Due::Nothing;
    if (m_due == Due::Welcome) {
        world.hideCrystals(); // Sumner reveals them once the scroll has gone
    }
}

void TowerWelcome::clear() {
    m_cutTicks = 0;
    m_due = Due::Nothing;
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
            if (runtime.life != PlayerLife::Standing) {
                continue;
            }
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
    const Due due = std::exchange(m_due, Due::Nothing);
    if (due == Due::Garm && !messages.open(device, kGarmMessage, strings)) {
        log::warn("Tower: no {} scroll to show", kGarmMessage);
    }
    if (due != Due::Welcome) {
        return;
    }
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
