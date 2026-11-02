#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "engine/math/PortableMath.h"

#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/LevelOpponents.h"
#include "game/hud/HelpMessages.h"
#include "game/players/ItemPickup.h"
#include "game/players/PickupVoices.h"

namespace gdl::game {
namespace {
constexpr f32 kLostLevelWait = 3.0f; ///< seconds the line waits behind narration (AudioExp)
} // namespace

bool LevelOpponents::releaseDeath(s32 record, const Vec3& position, s32 count) {
    if (!m_resources.has_value()) {
        return false;
    }
    const auto& infos = m_resources->world.layout().itemInfos();
    if (record < 0 || static_cast<usize>(record) >= infos.size()) {
        return false;
    }
    const ItemInfo& info = infos[static_cast<usize>(record)];
    if (info.type != ItemInfo::kPlacedEnemy || enemyKindOf(info.name) != kDeathKind ||
        !m_enemies.loadKind(kDeathKind)) {
        return false;
    }
    EnemySpawn spawn;
    spawn.kind = kDeathKind;
    spawn.tier = count != 0 ? 2 : 1;
    spawn.position = position;
    spawn.placed = true;
    return m_enemies.spawn(spawn, {}).has_value();
}

void LevelOpponents::stopDeathSound() {
    if (m_resources) {
        if (auto* imported = m_resources->world.imported()) {
            imported->setDeathContact(false);
        }
    }
    if (m_resources.has_value() && !m_resources->audio.recordingSounds()) {
        m_resources->audio.stop(m_deathSound);
        m_deathSound = kNoSound;
        m_resumeDeathSound = true;
    }
}

void LevelOpponents::clearDeaths() {
    if (m_resources.has_value()) {
        if (auto* imported = m_resources->world.imported()) {
            imported->setDeathContact(false);
        }
        for (const u32 id : m_sim.deathEffects) {
            m_resources->effects.stop(id);
        }
        if (!m_resources->audio.recordingSounds()) {
            m_resources->audio.stop(m_deathSound);
        }
    }
    m_sim.deathEffects.fill(0);
    m_deathSound = kNoSound;
    m_resumeDeathSound = false;
    m_sim.deathContact = false;
    m_sim.deathSound = {};
}

void LevelOpponents::gatherLoops() const {
    if (!m_resources || !m_resources->audio.recordingSounds()) {
        return;
    }
    auto& audio = m_resources->audio;
    if (m_sim.deathContact && m_sim.deathSound) {
        audio.desireNamedLoop({{20, Enemies::kMost, 1}, 1, 1}, "S_DEATHSUCK",
                              SceneSoundPlayback::PlayOnceUntilCancelled, 1, 0, m_sim.deathSound);
    }
    for (const auto& yell : m_sim.yells) {
        // The shared state keeps where the cry began; this machine's listener pans it.
        audio.desireNamedLoop({yell.emitter, 2, yell.instance}, "S_SUICIDE_YELL",
                              SceneSoundPlayback::PlayOnceUntilCancelled, yell.volume,
                              audio.panAt(yell.position), yell.start);
    }
}

void LevelOpponents::updateDeaths(std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    auto& resources = *m_resources;
    // Death's lessons go to the player he drains or who strikes him (enemy.c 1831, 5320,
    // 6021): his black form's are of experience, the rest of health.
    const auto teach = [&](const DeathEvent& cue, s32 experience, s32 health) {
        for (usize i = 0; i < players.size() && events.help; ++i) {
            if (players[i].actor.player() == cue.player) {
                events.help(cue.form == DeathForm::Black ? experience : health, i);
            }
        }
    };
    const auto consume = [&](const DeathEvent& cue) {
        if (resources.audio.recordingSounds() &&
            (cue.enemy < 0 || cue.enemy >= Enemies::kMost || cue.instance == 0)) {
            resources.audio.recorder()->unsupported("Death event without its source lifetime");
        }
        auto scope =
            resources.audio.soundScope({20, static_cast<u64>(std::max(cue.enemy, 0)), cue.instance},
                                       32 + static_cast<u32>(cue.kind));
        auto* imported = resources.world.imported();
        const bool nativeSound = imported != nullptr && imported->playDeathCue(cue);
        switch (cue.kind) {
        case DeathEvent::Kind::Killed:
            if (!nativeSound) {
                resources.audio.playNamed("S_DEATHDIE");
            }
            return;
        case DeathEvent::Kind::Exhausted:
            if (!nativeSound) {
                resources.audio.playNamed("S_DEATHLAUGH");
            }
            teach(cue, HelpMessages::kDeathLeavesExperience, HelpMessages::kDeathLeavesHealth);
            return;
        case DeathEvent::Kind::Awakened:
            if (!nativeSound) {
                resources.audio.playNamed("S_DEATHSHATTER");
            }
            return;
        case DeathEvent::Kind::Unmoved:
            teach(cue, HelpMessages::kUseMagicOnDeath, HelpMessages::kUseMagicOnDeath);
            return;
        case DeathEvent::Kind::Drain:
            teach(cue, HelpMessages::kDeathDrainsExperience, HelpMessages::kDeathDrainsHealth);
            break;
        default: break;
        }
        for (usize i = 0; i < players.size(); ++i) {
            PlayerRuntime& player = players[i];
            if (player.actor.player() != cue.player || player.life != PlayerLife::Standing) {
                continue;
            }
            auto& progress = player.actor.save().progress();
            const auto amount = static_cast<s32>(std::lround(cue.amount));
            // heal_player: no further than the level's most, 9999 at the top, whether the
            // health is his magic's (hurtDeath) or his own taken back through the halo.
            const auto heal = [&] {
                const s32 cap = mostHealth(experienceLevel(progress.experience));
                if (progress.health < cap) {
                    progress.health = std::min(cap, progress.health + amount);
                }
            };
            if (cue.form == DeathForm::Black && cue.kind != DeathEvent::Kind::MagicHeal) {
                const s32 before = experienceLevel(progress.experience);
                progress.experience =
                    std::max(0, progress.experience +
                                    (cue.kind == DeathEvent::Kind::Drain ? -amount : amount));
                if (experienceLevel(progress.experience) < before && player.figure != nullptr) {
                    // "<name> has lost a level" (AudioExp).
                    const CharacterSave& save = player.actor.save();
                    const std::array lines{std::string_view{"S_LOSTLEVEL"}};
                    resources.audio.announce(player.figure->voice(),
                                             PickupVoices::nameOf(save.character, save.color),
                                             PickupVoices::carriesPojo(save), lines, kLostLevelWait,
                                             AnnouncementSubject{save.character, save.color});
                }
            } else if (cue.kind == DeathEvent::Kind::MagicHeal ||
                       cue.kind == DeathEvent::Kind::Return) {
                heal();
            } else if (events.hurt) {
                events.hurt(i, cue.amount, HurtKind::DeathDrain, true,
                            PlayerImpact{.flags = damage::kDeathStun});
                if (player.life != PlayerLife::Standing) {
                    m_enemies.finishDeath(cue.enemy, cue.player);
                }
            }
            break;
        }
    };
    for (const DeathEvent& cue : m_enemies.takeDeathEvents()) {
        consume(cue);
    }
    // A lethal drain can enqueue the departure during the player-health callback.
    for (const DeathEvent& cue : m_enemies.takeDeathEvents()) {
        consume(cue);
    }
    bool contact = false;
    for (s32 i = 0; i < Enemies::kMost; ++i) {
        u32& effect = m_sim.deathEffects[static_cast<usize>(i)];
        if (!m_enemies.draining(i)) {
            resources.effects.stop(effect);
            effect = 0;
            continue;
        }
        contact = true;
        if (effect == 0) {
            ItemArchive* archive = m_enemies.archive(kDeathKind);
            if (archive != nullptr) {
                EffectTrees::Setting settings;
                settings.persistent = true;
                settings.depthWrite = false;
                effect = resources.effects.startSet(
                    resources.device, *archive,
                    DeathRules::effect(DeathRules::form(m_enemies.tierOf(i))),
                    m_enemies.positionOf(i), settings);
            }
        }
        const Mat4 transform = portable::rotate(glm::translate(Mat4{1}, m_enemies.positionOf(i)),
                                                m_enemies.yawOf(i), Vec3{0, 1, 0});
        resources.effects.placeAt(effect, transform);
    }
    auto* imported = resources.world.imported();
    if (imported && imported->setDeathContact(contact)) {
        m_sim.deathSound = {};
        m_resumeDeathSound = false;
    } else if (resources.audio.recordingSounds()) {
        if (contact && !m_sim.deathContact) {
            auto scope = resources.audio.soundScope({20, Enemies::kMost, 1}, 1);
            m_sim.deathSound = resources.audio.beginNamedCue("S_DEATHSUCK");
        } else if (!contact) {
            m_sim.deathSound = {};
        }
    } else {
        if (contact && (!m_sim.deathContact || m_resumeDeathSound)) {
            m_deathSound = resources.audio.playNamed("S_DEATHSUCK");
        } else if (!contact && m_sim.deathContact) {
            resources.audio.stop(m_deathSound);
            m_deathSound = kNoSound;
        }
        m_resumeDeathSound = false;
    }
    m_sim.deathContact = contact;
}
} // namespace gdl::game
