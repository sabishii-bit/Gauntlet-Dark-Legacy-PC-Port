#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/camera/CameraMovementLimit.h"
#include "game/combat/HurtKind.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/PowerupSelector.h"
#include "game/party/PartyMotion.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {
namespace {

constexpr s32 kTurboCrowd = 15;         ///< the swarm in view for the turbo lesson
constexpr f32 kStrongThrowScale = 2.0f; ///< a strong throw's weapon: twice the size and the harm
constexpr std::string_view kTaggedSound = "S_TAGGED";
constexpr f32 kTaggedVolume = 180.0f / 255.0f; ///< fn_8009DCB4

} // namespace

/** The scene's answers to what the party's movement asks as it steps: its cues, each seat's
 * selector and turbo move under that seat's sound scope, the attacks' queries, the opponents'
 * bodies, the shared view's hold, the level's start and the console's noclip. */
class PlayScene::MotionRouting final : public PartyMotion::Level {
public:
    explicit MotionRouting(PlayScene& scene) : m_scene(scene) {}
    void perform(usize i, PartyMotion::Action action) override { m_scene.perform(i, action); }
    void stepSelector(usize i, const SelectorInput& input, s32 elapsed) override {
        const auto audioScope = m_scene.m_audio.soundScope(
            {32, 0, 1, static_cast<u8>(m_scene.m_players[i].actor.player())}, 1);
        m_scene.m_hud.stepSelector(m_scene.m_players[i].actor, input, elapsed, m_scene.m_audio,
                                   m_scene.m_world->ref().isSecret());
    }
    void advanceTurbo(usize i, s32 elapsed, f32 duration) override {
        const auto audioScope = m_scene.m_audio.soundScope(
            {32, 0, 1, static_cast<u8>(m_scene.m_players[i].actor.player())}, 2);
        m_scene.m_attacks.updateTurbo(
            i, elapsed, duration, m_scene.m_players, [this](s32 id, usize index) {
                // The meter full, the lesson waits for fifteen in
                // view (pmotion.c 1756); the combo's for a party
                // of more than one besides (pmotion.c 1760).
                const bool crowd = m_scene.m_opponents.enemies().inView() >= kTurboCrowd;
                if (id == HelpMessages::kUseCombo) {
                    if (crowd && m_scene.standingCount() > 1) {
                        m_scene.postHelp(id, index);
                    }
                } else if (id != HelpMessages::kUseTurbo || crowd) {
                    m_scene.postHelp(id, index);
                }
            });
    }
    void thrownImpact(usize i, f32 damage) override {
        m_scene.hurt(i, damage, HurtKind::Blow, true);
    }
    std::optional<Vec3> aim(usize i) const override {
        return m_scene.assistedAim(i, m_scene.m_players[i].actor.facing());
    }
    Vec3 limitMovement(usize index, const Vec3& before, const Vec3& after) const override {
        return m_scene.limitStep(index, before, after);
    }
    PlayerDeed attackDeed(usize i, bool strong, bool moved, const Vec3& facing) const override {
        const s32 chain = m_scene.m_players[i].figure != nullptr
                              ? m_scene.m_players[i].figure->animator().meleeChain()
                              : 0;
        return m_scene.m_attacks.attackDeed(m_scene.m_players[i].actor, strong,
                                            m_scene.attackTargets(), moved, chain, facing);
    }
    PlayerDeed automaticMeleeDeed(usize i, const Vec3& facing) const override {
        if (m_scene.m_services->config == nullptr ||
            !m_scene.m_services->config->combat.autoMelee) {
            return PlayerDeed::None;
        }
        return m_scene.m_attacks.automaticMeleeDeed(m_scene.m_players[i].actor,
                                                    m_scene.attackTargets(), facing);
    }
    std::optional<MeleeSense> meleeSense(usize i, bool held, const Vec3& facing) const override {
        return m_scene.m_attacks.meleeSense(m_scene.m_players[i].actor, held,
                                            m_scene.attackTargets(), facing);
    }
    std::optional<Vec3> deathContact(usize i) const override {
        return m_scene.m_attacks.deathContact(i, m_scene.m_players, m_scene.attackTargets());
    }
    std::optional<Vec3> grabDeath(usize i, s32 ticks, bool allowed) override {
        return m_scene.m_attacks.grabDeath(i, ticks, allowed, m_scene.m_players,
                                           m_scene.attackTargets());
    }
    bool chainsHeldMelee() const override {
        return m_scene.m_services->config != nullptr &&
               m_scene.m_services->config->combat.autoMelee;
    }
    Vec3 resolveMovement(usize i, const Vec3& from, const Vec3& to) const override {
        return m_scene.m_opponents.resolveMovement(m_scene.m_players[i].actor, from, to);
    }
    std::optional<Vec3> startPoint() const override {
        if (const WorldLocator* start = m_scene.m_world->startPoint(0)) {
            return start->position;
        }
        return std::nullopt;
    }
    bool comboImpact(usize flier, usize thrower, f32 blow) override {
        return m_scene.m_attacks.comboImpact(flier, thrower, blow, m_scene.m_players,
                                             m_scene.attackTargets());
    }
    bool flying(usize i) const override {
        return m_scene.cheats(m_scene.m_players[i].actor.player()).noclip;
    }

private:
    PlayScene& m_scene;
};

std::vector<CameraSubject> PlayScene::stepParty(std::span<const PlayInput> inputs, bool held,
                                                std::span<const f32> yaws, s32 ticks, f32 seconds) {
    MotionRouting routing{*this};
    return PartyMotion::step(m_players, inputs, held, yaws, ticks, seconds, m_world->collision(),
                             routing);
}

/** How many of the party stand in the level. */
usize PlayScene::standingCount() const {
    return static_cast<usize>(std::ranges::count_if(m_players, [](const PlayerRuntime& player) {
        return player.life == PlayerLife::Standing;
    }));
}

/** Carries out what a member's movement set off: a throw, a swing, a potion, a footstep. */
void PlayScene::perform(usize i, PartyMotion::Action action) {
    const auto audioScope = m_audio.soundScope(
        {33, 0, 1, static_cast<u8>(m_players[i].actor.player())}, static_cast<u32>(action) + 1);
    switch (action) {
    case PartyMotion::Action::NoPotion: postHelp(HelpMessages::kNoPotion, i); break;
    case PartyMotion::Action::Ram: m_attacks.ramBarrels(i, m_players, attackTargets()); break;
    case PartyMotion::Action::ThrowWeapon: throwWeapon(m_players[i].actor); break;
    case PartyMotion::Action::FamiliarShot:
        m_arsenal.launchFamiliar(m_players[i].actor, m_players[i].figure.get(),
                                 assistedAim(i, m_players[i].actor.facing()),
                                 m_players[i].cursorAiming);
        break;
    case PartyMotion::Action::StrongThrow:
        launchWeapon(i, m_players[i].actor.facing(), kStrongThrowScale, true);
        break;
    case PartyMotion::Action::SuperShot:
        m_arsenal.launchSuperShot(
            m_players[i].actor, m_players[i].figure.get(),
            m_players[i].cursorAiming ? std::nullopt : assistedAim(i, m_players[i].actor.facing()));
        break;
    case PartyMotion::Action::ShieldPotion: m_attacks.shieldPotion(i, m_players); break;
    case PartyMotion::Action::ItemAttack: m_attacks.useItemAttack(i, m_players); break;
    case PartyMotion::Action::UsePotion: m_attacks.usePotion(i, m_players); break;
    case PartyMotion::Action::ThrowPotion:
        m_arsenal.throwPotion(
            m_players[i].actor,
            m_players[i].figure != nullptr ? m_players[i].figure->animator().potionThrowTicks() : 0,
            m_players[i].figure != nullptr ? m_players[i].figure->animator().player().generation()
                                           : 0);
        break;
    case PartyMotion::Action::FirstFoot:
    case PartyMotion::Action::SecondFoot: {
        const auto& actor = m_players[i].actor;
        const auto water = m_world->collision().liquidAt(actor.position(), actor.height(), 0);
        m_audio.playFootstep(
            action == PartyMotion::Action::SecondFoot,
            LevelSoundscape::footingOf(m_players[i].floor.flags,
                                       PowerupEffects::of(actor.save().progress().inventory).armor,
                                       water && water->y > actor.position().y));
        break;
    }
    case PartyMotion::Action::Melee: m_attacks.melee(i, m_players, attackTargets()); break;
    case PartyMotion::Action::Tagged:
        postHelp(HelpMessages::kNowIt, i);
        m_audio.playNamed(kTaggedSound, kTaggedVolume);
        break;
    case PartyMotion::Action::ComboStart: m_attacks.comboStart(i, m_players); break;
    case PartyMotion::Action::Fallen:
        // Silent: a dropped peer's seat comes here between recorded ticks (waitInTower).
        m_players[i].mikey.clear();
        m_players[i].mikeyFigure.reset();
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
    // Each seat's own camera travels with them: nothing holds the party together.
    if (soloViewActive()) {
        return after;
    }
    // A lone player's follow camera can travel with them. The shared
    // view must constrain separation; fixed boss views also need bounds.
    if (!bossCameraOn() && std::ranges::count_if(m_players, [](const PlayerRuntime& player) {
                               return player.life == PlayerLife::Standing;
                           }) <= 1) {
        return after;
    }
    // Use the unshaken gameplay camera, never the promotion/victory cut.
    auto camera = bossCameraOn() ? m_sim.bossCamera.camera() : m_camera.camera();
    auto attention = bossCameraOn() ? m_sim.bossCamera.attention() : m_camera.attention();
    if (const auto* imported = m_world->imported()) {
        if (const auto native = imported->camera()) {
            camera = *native;
            // Imported cameras have their own filtered focus. In particular,
            // behind-camera recovery must not target the unused GDL camera.
            attention = imported->cameraAttention().value_or(attention);
        }
    }
    const auto& actor = m_players[index].actor;
    return CameraMovementLimit::constrain(before, after, attention, camera, cameraView(),
                                          actor.followPoint() - actor.position());
}

} // namespace gdl::game
