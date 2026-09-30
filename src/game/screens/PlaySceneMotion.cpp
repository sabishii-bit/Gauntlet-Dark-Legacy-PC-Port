#include <algorithm>
#include <optional>

#include "engine/core/Types.h"

#include "game/screens/HelpMessages.h"
#include "game/screens/PlayScene.h"
#include "game/world/CameraMovementLimit.h"

namespace gdl::game {

namespace {

constexpr s32 kTurboCrowd = 15;         ///< the swarm in view for the turbo lesson
constexpr f32 kStrongThrowScale = 2.0f; ///< a strong throw's weapon: twice the size and the harm
constexpr std::string_view kTaggedSound = "S_TAGGED";
constexpr f32 kTaggedVolume = 180.0f / 255.0f; ///< fn_8009DCB4

} // namespace

/** What the party's movement asks of the scene as it steps. */
PartyMotion::Events PlayScene::motionEvents() {
    return {.perform = [this](usize i, PartyMotion::Action action) { perform(i, action); },
            .select =
                [this](usize i, const SelectorInput& input, s32 elapsed) {
                    m_hud.stepSelector(m_players[i].actor, input, elapsed, m_audio);
                },
            .advanceTurbo =
                [this](usize i, s32 elapsed, f32 duration) {
                    m_attacks.updateTurbo(
                        i, elapsed, duration, m_players, [this](s32 id, usize index) {
                            // The meter full, the lesson waits for fifteen in
                            // view (pmotion.c 1756); the combo's for a party
                            // of more than one besides (pmotion.c 1760).
                            const bool crowd = m_opponents.enemies().inView() >= kTurboCrowd;
                            if (id == HelpMessages::kUseCombo) {
                                if (crowd && standingCount() > 1) {
                                    postHelp(id, index);
                                }
                            } else if (id != HelpMessages::kUseTurbo || crowd) {
                                postHelp(id, index);
                            }
                        });
                },
            .thrownImpact = [this](usize i, f32 damage) { hurt(i, damage, HurtKind::Blow, true); },
            .aim =
                [this](usize i) {
                    const PlayerActor& actor = m_players[i].actor;
                    return m_attacks.aim(actor, actor.facing(), attackTargets());
                },
            .limitMovement = [this](usize index, const Vec3& before,
                                    const Vec3& after) { return limitStep(index, before, after); },
            .attackDeed =
                [this](usize i, bool strong, bool moved) {
                    const s32 chain = m_players[i].figure != nullptr
                                          ? m_players[i].figure->animator().meleeChain()
                                          : 0;
                    return m_attacks.attackDeed(m_players[i].actor, strong, attackTargets(), moved,
                                                chain);
                },
            .meleeSense =
                [this](usize i, bool held) {
                    return m_attacks.meleeSense(m_players[i].actor, held, attackTargets());
                },
            .grabDeath =
                [this](usize i, s32 ticks, bool allowed) {
                    return m_attacks.grabDeath(i, ticks, allowed, m_players, attackTargets());
                },
            .resolveMovement =
                [this](usize i, const Vec3& from, const Vec3& to) {
                    return m_opponents.resolveMovement(m_players[i].actor, from, to);
                },
            .startPoint = [this]() -> std::optional<Vec3> {
                if (const WorldLocator* start = m_world->startPoint(0)) {
                    return start->position;
                }
                return std::nullopt;
            },
            .comboImpact =
                [this](usize flier, usize thrower, f32 blow) {
                    return m_attacks.comboImpact(flier, thrower, blow, m_players, attackTargets());
                }};
}

/** How many of the party stand in the level. */
usize PlayScene::standingCount() const {
    return static_cast<usize>(std::ranges::count_if(m_players, [](const PlayerRuntime& player) {
        return player.life == PlayerLife::Standing;
    }));
}

/** Carries out what a member's movement set off: a throw, a swing, a potion, a footstep. */
void PlayScene::perform(usize i, PartyMotion::Action action) {
    switch (action) {
    case PartyMotion::Action::NoPotion: postHelp(HelpMessages::kNoPotion, i); break;
    case PartyMotion::Action::Ram: m_attacks.ramBarrels(i, m_players, attackTargets()); break;
    case PartyMotion::Action::ThrowWeapon: throwWeapon(m_players[i].actor); break;
    case PartyMotion::Action::FamiliarShot:
        m_arsenal.launchFamiliar(
            m_players[i].actor, m_players[i].figure.get(),
            m_attacks.aim(m_players[i].actor, m_players[i].actor.facing(), attackTargets()));
        break;
    case PartyMotion::Action::StrongThrow:
        launchWeapon(i, m_players[i].actor.facing(), kStrongThrowScale, true);
        break;
    case PartyMotion::Action::SuperShot:
        m_arsenal.launchSuperShot(
            m_players[i].actor, m_players[i].figure.get(),
            m_attacks.aim(m_players[i].actor, m_players[i].actor.facing(), attackTargets()));
        break;
    case PartyMotion::Action::ShieldPotion: m_attacks.shieldPotion(i, m_players); break;
    case PartyMotion::Action::ItemAttack: m_attacks.useItemAttack(i, m_players); break;
    case PartyMotion::Action::UsePotion: m_attacks.usePotion(i, m_players); break;
    case PartyMotion::Action::ThrowPotion: m_arsenal.throwPotion(m_players[i].actor); break;
    case PartyMotion::Action::FirstFoot:
    case PartyMotion::Action::SecondFoot:
        m_audio.playFootstep(
            action == PartyMotion::Action::SecondFoot,
            LevelSoundscape::footingOf(
                m_players[i].floor.flags,
                PowerupEffects::of(m_players[i].actor.save().progress().inventory).armor));
        break;
    case PartyMotion::Action::Melee: m_attacks.melee(i, m_players, attackTargets()); break;
    case PartyMotion::Action::Tagged:
        postHelp(HelpMessages::kNowIt, i);
        m_audio.playNamed(kTaggedSound, kTaggedVolume);
        break;
    case PartyMotion::Action::ComboStart: m_attacks.comboStart(i, m_players); break;
    case PartyMotion::Action::Fallen:
        if (m_device != nullptr) {
            PartyPickups::dropKeys(*m_device, *m_world, m_players[i]);
        }
        // Out of the tower the fallen may wait there or leave (inactivate_player).
        m_players[i].towerPrompt = !m_world->isTower();
        break;
    }
}

/** Whether a step keeps the party within the shared view. */
Vec3 PlayScene::limitStep(usize index, const Vec3& before, const Vec3& after) const {
    // A lone player's follow camera can travel with them. The shared
    // view must constrain separation; fixed boss views also need bounds.
    if (!bossCameraOn() && std::ranges::count_if(m_players, [](const PlayerRuntime& player) {
                               return player.life == PlayerLife::Standing;
                           }) <= 1) {
        return after;
    }
    // Use the unshaken gameplay camera, never the promotion/victory cut.
    const auto& camera = bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
    const auto& attention = bossCameraOn() ? m_bossCamera.attention() : m_camera.attention();
    const auto& actor = m_players[index].actor;
    return CameraMovementLimit::constrain(before, after, attention, camera, cameraView(),
                                          actor.followPoint() - actor.position());
}

} // namespace gdl::game
