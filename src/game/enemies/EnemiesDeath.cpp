#include <algorithm>

#include "game/enemies/Enemies.h"

namespace gdl::game {
void Enemies::drain(Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players) {
    const EnemyView* victim = viewOf(players, enemy.contact);
    enemy.draining = false;
    if (enemy.state != State::Active || victim == nullptr || victim->hidden || victim->antiDeath ||
        enemy.stunTicks > 0 || enemy.animator.entering()) {
        enemy.drainTicks = std::max(0, enemy.drainTicks - ticks);
        return;
    }
    if (enemy.endurance > 0) {
        --enemy.endurance;
        return;
    }
    enemy.draining = true;
    enemy.drainTicks -= ticks;
    if (enemy.drainTicks > 0) {
        return;
    }
    enemy.drainTicks += DeathRules::kDrainTicks;
    const DeathForm form = DeathRules::form(enemy.tier);
    const f32 amount = form == DeathForm::Black
                           ? static_cast<f32>(DeathRules::experience(victim->level, true))
                           : fightOf(enemy);
    m_deathEvents.push_back(
        {DeathEvent::Kind::Drain, slot, victim->player, form, amount, enemy.position});
    enemy.health -= fightOf(enemy);
    if (enemy.health < 0) {
        finishDeath(slot);
    }
}

void Enemies::hurtDeath(Enemy& enemy, s32 slot, const EnemyHit& hit) {
    const DeathForm form = DeathRules::form(enemy.tier);
    if ((hit.flags & EnemyHit::kMagic) != 0) {
        const f32 heal = DeathRules::magicHealing(hit.level, enemy.health);
        if (hit.player >= 0 && heal > 0) {
            m_deathEvents.push_back(
                {DeathEvent::Kind::MagicHeal, slot, hit.player, form, heal, enemy.position});
        }
        enemy.health = 0;
    } else if (enemy.state == State::Asleep) {
        if (--enemy.endurance <= 0) {
            enemy.state = State::Active;
            m_deathEvents.push_back(
                {DeathEvent::Kind::Awakened, slot, hit.player, form, 0, enemy.position});
        }
        return;
    } else {
        enemy.health -= 1;
        if (hit.player >= 0 && hit.antiDeath) {
            const f32 amount = form == DeathForm::Black
                                   ? static_cast<f32>(DeathRules::experience(hit.level, false))
                                   : 1;
            m_deathEvents.push_back(
                {DeathEvent::Kind::Return, slot, hit.player, form, amount, enemy.position});
        }
    }
    if (enemy.health <= 0) {
        m_deathEvents.push_back(
            {DeathEvent::Kind::Killed, slot, hit.player, form, 0, enemy.position});
        if (hit.player >= 0) {
            m_losses.push_back({slot, enemy.kind, enemy.tier, hit.player, 0, true, enemy.position});
        }
        enemy.state = State::Dying;
        enemy.killed = true;
        enemy.draining = false;
    }
}

bool Enemies::draining(s32 id) const {
    return alive(id) && m_enemies[static_cast<usize>(id)].draining;
}

void Enemies::finishDeath(s32 id) {
    if (!alive(id) || kindOf(id) != kDeathKind) {
        return;
    }
    Enemy& enemy = m_enemies[static_cast<usize>(id)];
    m_deathEvents.push_back(
        {DeathEvent::Kind::Exhausted, id, -1, DeathRules::form(enemy.tier), 0, enemy.position});
    enemy.health = 0;
    enemy.state = State::Dying;
    enemy.killed = true;
    enemy.draining = false;
}

std::vector<DeathEvent> Enemies::takeDeathEvents() {
    std::vector<DeathEvent> out;
    out.swap(m_deathEvents);
    return out;
}
} // namespace gdl::game
