#include "game/tower/TowerWelcome.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/assets/StringTable.h"
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

void TowerWelcome::open(LevelWorld& world, bool welcome, std::optional<LevelBeaten> beaten,
                        bool gateways) {
    clear();
    if (welcome) {
        m_due = Due::Welcome;
        world.hideCrystals(); // Sumner reveals them once the scroll has gone
        return;
    }
    const Due told = gateways ? Due::Gateways : Due::Nothing;
    if (beaten == kUnderworldThrone) {
        m_due = Due::Garm;
        m_next = told;
    } else {
        m_due = told;
    }
}

void TowerWelcome::clear() {
    m_cutTicks = 0;
    m_due = Due::Nothing;
    m_next = Due::Nothing;
    m_intro = Intro::None;
}

bool TowerWelcome::freshParty(std::span<const PartyMember> party) {
    return !party.empty() && std::ranges::all_of(party, [](const PartyMember& member) {
        return std::ranges::none_of(member.save.classes, [](const ClassProgress& progress) {
            return progress.experience > 0;
        });
    });
}

bool TowerWelcome::owesGateways(std::span<const PartyMember> party, TowerGateways* gateways) {
    const bool untold = std::ranges::any_of(party, [](const PartyMember& member) {
        return beatenGarm(member.save) && !member.save.gatewaysAnnounced;
    });
    // Nothing is looked for on disk for a party none of whom is owed it.
    return gateways != nullptr && untold &&
           std::ranges::any_of(gateways->worlds(), &GatewayWorld::usable);
}

usize TowerWelcome::tellGateways(std::span<PlayerRuntime> players) {
    usize told = 0;
    for (PlayerRuntime& runtime : players) {
        CharacterSave& save = runtime.actor.save();
        if (runtime.departed || !beatenGarm(save)) {
            continue;
        }
        if (!save.gatewaysAnnounced) {
            ++told;
        }
        save.gatewaysAnnounced = true;
        runtime.entrySave.gatewaysAnnounced = true;
    }
    return told;
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

TowerWelcome::Due TowerWelcome::arrived(RenderDevice& device, LevelMessages& messages,
                                        const StringTable* strings, const WorldLayout& layout,
                                        SumnerFigure& sumner) {
    return openDue(device, messages, strings, layout, sumner);
}

TowerWelcome::Due TowerWelcome::scrollClosed(RenderDevice& device, LevelMessages& messages,
                                             const StringTable* strings, const WorldLayout& layout,
                                             SumnerFigure& sumner) {
    if (m_intro == Intro::Scroll) {
        startCrystalCut(layout, sumner);
        return Due::Nothing;
    }
    return openDue(device, messages, strings, layout, sumner);
}

TowerWelcome::Due TowerWelcome::openDue(RenderDevice& device, LevelMessages& messages,
                                        const StringTable* strings, const WorldLayout& layout,
                                        SumnerFigure& sumner) {
    while (m_due != Due::Nothing) {
        const Due due = std::exchange(m_due, std::exchange(m_next, Due::Nothing));
        switch (due) {
        case Due::Welcome:
            if (messages.open(device, kWelcomeMessage, strings)) {
                m_intro = Intro::Scroll;
                return due;
            }
            log::warn("Tower: no welcome scroll to show; on to the crystals");
            startCrystalCut(layout, sumner);
            return Due::Nothing;
        case Due::Garm:
            if (messages.open(device, kGarmMessage, strings)) {
                return due;
            }
            log::warn("Tower: no {} scroll to show", kGarmMessage);
            break;
        case Due::Gateways:
            // Drawn as GarmMessage is, the arrival's scroll before it.
            if (strings != nullptr && strings->has(kGatewayScroll) &&
                messages.openStyled(device, kGarmMessage,
                                    {std::string(strings->get(kGatewayScroll))}, strings)) {
                return due;
            }
            log::warn("Tower: no scroll to tell of Sumner's gateways; a later arrival tells it");
            break;
        case Due::Nothing: break;
        }
    }
    return Due::Nothing;
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
