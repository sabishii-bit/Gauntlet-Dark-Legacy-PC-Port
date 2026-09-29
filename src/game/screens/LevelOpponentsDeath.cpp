#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "game/players/PickupVoices.h"
#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/LevelOpponents.h"

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

void LevelOpponents::clearDeaths() {
    if (m_resources.has_value()) {
        for (const u32 id : m_deathEffects) {
            m_resources->effects.stop(id);
        }
        m_resources->audio.stop(m_deathSound);
    }
    m_deathEffects.fill(0);
    m_deathSound = kNoSound;
    m_deathContact = false;
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
        switch (cue.kind) {
        case DeathEvent::Kind::Killed: resources.audio.playNamed("S_DEATHDIE"); return;
        case DeathEvent::Kind::Exhausted:
            resources.audio.playNamed("S_DEATHLAUGH");
            teach(cue, HelpMessages::kDeathLeavesExperience, HelpMessages::kDeathLeavesHealth);
            return;
        case DeathEvent::Kind::Awakened: resources.audio.playNamed("S_DEATHSHATTER"); return;
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
            if (cue.kind == DeathEvent::Kind::MagicHeal) {
                const s32 cap = std::min(
                    9999, kStartingHealth + 100 * (experienceLevel(progress.experience) - 1));
                if (progress.health < cap) {
                    progress.health = std::min(cap, progress.health + amount);
                }
            } else if (cue.form == DeathForm::Black) {
                const s32 before = experienceLevel(progress.experience);
                progress.experience =
                    std::max(0, progress.experience +
                                    (cue.kind == DeathEvent::Kind::Drain ? -amount : amount));
                if (experienceLevel(progress.experience) < before && player.figure != nullptr) {
                    // "<name> has lost a level" (AudioExp).
                    const CharacterSave& save = player.actor.save();
                    const std::array lines{std::string_view{"S_LOSTLEVEL"}};
                    resources.audio.announce(
                        player.figure->voice(), PickupVoices::nameOf(save.character, save.color),
                        PickupVoices::carriesPojo(save), lines, kLostLevelWait);
                }
            } else if (cue.kind == DeathEvent::Kind::Return) {
                progress.health += amount;
            } else if (events.hurt) {
                events.hurt(i, cue.amount, HurtKind::DeathDrain, true,
                            PlayerImpact{.flags = 0x1000});
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
        u32& effect = m_deathEffects[static_cast<usize>(i)];
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
        const Mat4 transform = glm::rotate(glm::translate(Mat4{1}, m_enemies.positionOf(i)),
                                           m_enemies.yawOf(i), Vec3{0, 1, 0});
        resources.effects.placeAt(effect, transform);
    }
    if (contact && !m_deathContact) {
        m_deathSound = resources.audio.playNamed("S_DEATHSUCK");
    } else if (!contact && m_deathContact) {
        resources.audio.stop(m_deathSound);
        m_deathSound = kNoSound;
    }
    m_deathContact = contact;
}
} // namespace gdl::game
