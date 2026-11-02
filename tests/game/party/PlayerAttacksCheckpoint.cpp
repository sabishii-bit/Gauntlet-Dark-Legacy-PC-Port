#include "game/party/PlayerAttacksCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_set>

#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"

namespace gdl::game {
namespace {
using State = PlayerAttacksCheckpoint::State;
using Item = PlayerAttacksCheckpoint::Item;
bool finite(f32 value) {
    return std::isfinite(value) && std::abs(value) <= 1e6f;
}
bool finite(const Vec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
bool affine(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!finite(value[column][row])) {
                return false;
            }
        }
    }
    return value[0][3] == 0 && value[1][3] == 0 && value[2][3] == 0 && value[3][3] == 1;
}
std::optional<ItemAttack> descriptor(Item item, bool pojo) {
    PowerupEffects worn;
    switch (item) {
    case Item::Horns: worn.special = powerup::kSkorneHorns; break;
    case Item::Mask: worn.special = powerup::kSkorneMask; break;
    case Item::LeftGauntlet: worn.special = powerup::kLeftGauntlet; break;
    case Item::RightGauntlet: worn.special = powerup::kRightGauntlet; break;
    case Item::SuperShot: worn.weapon = damage::kSuperShot; break;
    case Item::Hammer: worn.weapon = damage::kHammer; break;
    case Item::FireBreath: worn.special = powerup::kFireBreath; break;
    case Item::AcidBreath: worn.special = powerup::kAcidBreath; break;
    case Item::LightningBreath: worn.special = powerup::kLightningBreath; break;
    default: return std::nullopt;
    }
    auto attack = ItemAttack::select(worn);
    if (pojo) {
        if (item != Item::Horns && item != Item::Mask && item != Item::FireBreath) {
            return std::nullopt;
        }
        attack->chargeKind = 0;
    }
    return attack;
}
bool same(const ItemAttack& left, const ItemAttack& right) {
    return std::tie(left.deed, left.tree, left.sound, left.damage, left.radius, left.delay,
                    left.minDot, left.flags, left.chargeKind, left.chargeMask, left.head) ==
           std::tie(right.deed, right.tree, right.sound, right.damage, right.radius, right.delay,
                    right.minDot, right.flags, right.chargeKind, right.chargeMask, right.head);
}
std::optional<Item> identify(const ItemAttack& attack, bool pojo) {
    for (u8 value = 0; value <= static_cast<u8>(Item::LightningBreath); ++value) {
        const auto item = static_cast<Item>(value);
        if (const auto selected = descriptor(item, pojo); selected && same(*selected, attack)) {
            return item;
        }
    }
    return std::nullopt;
}
bool resourcesValid(std::span<const PlayerMissiles::Resource> resources) {
    if (resources.size() > PlayerMissiles::Checkpoint::kMaxResources) {
        return false;
    }
    std::unordered_set<u32> ids;
    for (const auto& resource : resources) {
        if (resource.id == 0 || resource.spec == nullptr || !ids.insert(resource.id).second) {
            return false;
        }
    }
    return true;
}
bool impactValid(const PlayerMissiles::Checkpoint::Impact& impact) {
    return finite(impact.position) && finite(impact.direction) && impact.owner >= -1 &&
           impact.owner < 4 && impact.potion >= 0 && impact.potion <= 4 && finite(impact.potency) &&
           finite(impact.damage) &&
           static_cast<u8>(impact.wallSound) <= static_cast<u8>(MissileWallSound::Ricochet) &&
           impact.node >= -1 && impact.worldObject >= -1 && impact.player >= -1 &&
           impact.player < 4 && finite(impact.playerHitGap) && impact.playerHitGap >= 0;
}
std::optional<PlayerMissiles::Checkpoint::Impact>
captureImpact(const MissileImpact& impact, std::span<const PlayerMissiles::Resource> resources) {
    u32 id = 0;
    if (!impact.effect.empty()) {
        const auto resource = std::ranges::find_if(
            resources, [&](const auto& entry) { return entry.spec->impactTree == impact.effect; });
        if (resource == resources.end()) {
            return std::nullopt;
        }
        id = resource->id;
    }
    return PlayerMissiles::Checkpoint::Impact{impact.position,
                                              impact.owner,
                                              impact.potion,
                                              impact.potency,
                                              impact.damage,
                                              impact.target,
                                              id,
                                              impact.wallSound,
                                              impact.flags,
                                              impact.node,
                                              impact.worldObject,
                                              impact.liquid,
                                              impact.player,
                                              impact.direction,
                                              impact.stun,
                                              impact.playerHitGap};
}
std::optional<MissileImpact> restoreImpact(const PlayerMissiles::Checkpoint::Impact& impact,
                                           std::span<const PlayerMissiles::Resource> resources) {
    std::string_view effect;
    if (impact.effectResource != 0) {
        const auto resource =
            std::ranges::find(resources, impact.effectResource, &PlayerMissiles::Resource::id);
        if (resource == resources.end() || resource->spec->impactTree.empty()) {
            return std::nullopt;
        }
        effect = resource->spec->impactTree;
    }
    return MissileImpact{
        impact.position,    impact.owner,  impact.potion,    impact.potency,   impact.damage,
        impact.target,      effect,        impact.wallSound, impact.flags,     impact.node,
        impact.worldObject, impact.liquid, impact.player,    impact.direction, impact.stun,
        impact.playerHitGap};
}
} // namespace

bool PlayerAttacksCheckpoint::State::valid() const {
    if (actors.size() > 4 || !strikes.valid() || items.size() > kMaxAreas ||
        potions.size() > kMaxAreas || strikeEffects.size() > kMaxAreas ||
        strikeSources.size() > kMaxAreas || attachments.size() > kMaxAreas ||
        particles.size() > kMaxAreas || shields.size() > kMaxAreas ||
        (!bound && (!strikes.strikes.empty() || !items.empty() || !potions.empty() ||
                    !strikeEffects.empty() || !strikeSources.empty() || !attachments.empty() ||
                    !particles.empty() || !shields.empty()))) {
        return false;
    }
    std::unordered_set<s32> slots;
    for (const auto& actor : actors) {
        if (actor.player < 0 || actor.player >= 4 || actor.character < 0 || actor.character > 16 ||
            !slots.insert(actor.player).second) {
            return false;
        }
    }
    usize contacts = 0;
    const auto ledger = [&](const std::vector<s32>& values) {
        if (values.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += values.size();
        std::unordered_set<s32> ids;
        return std::ranges::all_of(values, [&](s32 id) { return ids.insert(id).second; });
    };
    for (const auto& area : items) {
        if (!descriptor(area.item, area.pojoTurbo) || area.actor >= actors.size() ||
            !finite(area.elapsed) || area.elapsed < 0 || !finite(area.lifetime) ||
            area.lifetime <= 0 || !ledger(area.hit)) {
            return false;
        }
    }
    for (const auto& potion : potions) {
        if (!impactValid(potion.impact) ||
            (potion.impact.owner >= 0 && !slots.contains(potion.impact.owner)) ||
            !finite(potion.elapsed) || potion.elapsed < 0 || !finite(potion.duration) ||
            potion.duration <= 0 || !ledger(potion.hit) || !ledger(potion.blessed)) {
            return false;
        }
    }
    std::unordered_set<u32> sources;
    for (const auto& source : strikeSources) {
        if (source.strike == 0 || !sources.insert(source.strike).second ||
            source.actor >= actors.size() || source.row < 0 ||
            static_cast<u8>(source.multiplayer) > static_cast<u8>(MultiplayerMode::Hurt) ||
            source.contacts.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += source.contacts.size();
        std::unordered_set<s32> ids;
        for (const auto& contact : source.contacts) {
            if (!finite(contact.remaining) || !ids.insert(contact.target).second) {
                return false;
            }
        }
    }
    for (const auto& effect : strikeEffects) {
        if (effect.strike == 0 || effect.effect == 0) {
            return false;
        }
    }
    for (const auto& attachment : attachments) {
        if (attachment.actor >= actors.size() || !affine(attachment.local) ||
            (attachment.effect == 0 && attachment.strike == 0)) {
            return false;
        }
    }
    for (const auto& particle : particles) {
        if (particle.actor >= actors.size() || particle.node.size() > 256 ||
            particle.node.find('\0') != std::string::npos || !finite(particle.offset) ||
            (particle.effect != 0 && particle.effect == particle.parent)) {
            return false;
        }
    }
    for (const auto& shield : shields) {
        if (shield.actor >= actors.size() || !finite(shield.radius) || shield.radius < 0 ||
            !finite(shield.damage) || !finite(shield.secondsLeft) || !ledger(shield.blessed) ||
            shield.marks.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += shield.marks.size();
        std::unordered_set<u64> marked;
        for (const auto& mark : shield.marks) {
            // Native kinds are swarm, critter, generator, boss, and barrel.
            // These are historical IDs: a target may disappear before its mark
            // expires. Zero wait is harmless and expires at the next update.
            if (mark.kind < 0 || mark.kind > 4 || mark.id < 0 || (mark.kind == 3 && mark.id != 0) ||
                !finite(mark.wait) || mark.wait < 0 || mark.wait > PlayerAttacks::kShieldHitAgain ||
                !marked.insert((static_cast<u64>(mark.kind) << 32) | static_cast<u32>(mark.id))
                     .second) {
                return false;
            }
        }
    }
    return true;
}

struct PlayerAttacksCheckpoint::Prepared::Data {
    const PlayerAttacks* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::optional<MoveStrikes::Prepared> strikes;
    std::vector<PlayerAttacks::ItemArea> items;
    std::vector<PlayerAttacks::PotionBurst> potions;
    std::vector<PlayerAttacks::StrikeEffect> strikeEffects;
    std::vector<PlayerAttacks::StrikeSource> strikeSources;
    std::vector<PlayerAttacks::MoveAttachment> attachments;
    std::vector<PlayerAttacks::ParticleEffect> particles;
    std::vector<PlayerAttacks::PotionShield> shields;
};
PlayerAttacksCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlayerAttacksCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
PlayerAttacksCheckpoint::Prepared&
PlayerAttacksCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
PlayerAttacksCheckpoint::Prepared::~Prepared() = default;

std::optional<PlayerAttacksCheckpoint::State>
PlayerAttacksCheckpoint::capture(const PlayerAttacks& owner, std::span<const Actor> actors,
                                 std::span<const PlayerMissiles::Resource> resources) {
    if (actors.size() > 4 || !resourcesValid(resources) ||
        owner.m_sim.items.size() > State::kMaxAreas ||
        owner.m_sim.potions.size() > State::kMaxAreas ||
        owner.m_sim.strikeEffects.size() > State::kMaxAreas ||
        owner.m_sim.strikeSources.size() > State::kMaxAreas ||
        owner.m_sim.moveAttachments.size() > State::kMaxAreas ||
        owner.m_sim.particleEffects.size() > State::kMaxAreas ||
        owner.m_sim.shields.size() > State::kMaxAreas) {
        return std::nullopt;
    }
    const auto strikes = owner.m_strikes.checkpoint();
    if (!strikes) {
        return std::nullopt;
    }
    State state;
    state.bound = owner.m_resources.has_value();
    state.actors.assign(actors.begin(), actors.end());
    state.strikes = *strikes;
    for (const auto& area : owner.m_sim.items) {
        const auto item = identify(area.attack, area.pojoTurbo);
        if (!item) {
            return std::nullopt;
        }
        state.items.push_back({*item, area.actor, area.effect, area.elapsed, area.lifetime,
                               area.pojoTurbo, area.hit});
    }
    for (const auto& potion : owner.m_sim.potions) {
        const auto impact = captureImpact(potion.impact, resources);
        if (!impact) {
            return std::nullopt;
        }
        state.potions.push_back(
            {*impact, potion.elapsed, potion.duration, potion.hit, potion.blessed, potion.struck});
    }
    for (const auto& effect : owner.m_sim.strikeEffects) {
        state.strikeEffects.push_back({effect.strike, effect.effect});
    }
    for (const auto& source : owner.m_sim.strikeSources) {
        State::StrikeSource copied{source.strike,      source.actor,     source.row, {},
                                   source.multiplayer, source.damageType};
        for (const auto& contact : source.contacts) {
            copied.contacts.push_back({contact.target, contact.remaining});
        }
        state.strikeSources.push_back(std::move(copied));
    }
    for (const auto& attachment : owner.m_sim.moveAttachments) {
        state.attachments.push_back({attachment.actor, attachment.effect, attachment.strike,
                                     attachment.local, attachment.bodyParent});
    }
    for (const auto& particle : owner.m_sim.particleEffects) {
        state.particles.push_back(
            {particle.actor, particle.effect, particle.parent, particle.node, particle.offset});
    }
    for (const auto& shield : owner.m_sim.shields) {
        if (shield.marks.size() > State::kMaxContacts) {
            return std::nullopt;
        }
        State::Shield copied{
            shield.actor,       shield.flags, shield.effect, shield.radius, shield.damage,
            shield.secondsLeft, {},           shield.blessed};
        copied.marks.reserve(shield.marks.size());
        for (const auto& mark : shield.marks) {
            copied.marks.push_back({mark.kind, mark.id, mark.wait});
        }
        state.shields.push_back(std::move(copied));
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

std::optional<PlayerAttacksCheckpoint::Prepared>
PlayerAttacksCheckpoint::prepare(PlayerAttacks& owner, const State& state,
                                 std::span<const Actor> actors,
                                 const EffectTrees::PoolCheckpoint& effects,
                                 std::span<const PlayerMissiles::Resource> resources) {
    if (!state.valid() || !effects.valid() || !resourcesValid(resources) || actors.size() > 4 ||
        state.bound != owner.m_resources.has_value() || owner.m_restoreGeneration == 0 ||
        !std::ranges::equal(state.actors, actors)) {
        return std::nullopt;
    }
    std::unordered_set<s32> slots;
    for (const auto& actor : actors) {
        if (actor.player < 0 || actor.player >= 4 || actor.character < 0 || actor.character > 16 ||
            !slots.insert(actor.player).second) {
            return std::nullopt;
        }
    }
    // Stopped visuals can remain in attachment lists until their native cleanup
    // phase. Validate allocated identity, not current playing(), and retain them.
    const auto effect = [&](u32 id) {
        return id == 0 || effects.nextId == 0 || id < effects.nextId;
    };
    const auto strike = [&](u32 id) {
        return id == 0 || state.strikes.next == 0 || id < state.strikes.next;
    };
    auto prepared = std::make_unique<Prepared::Data>();
    prepared->owner = &owner;
    prepared->lifetime = owner.m_restoreOwner;
    prepared->generation = owner.m_restoreGeneration;
    prepared->strikes = owner.m_strikes.prepareRestore(state.strikes);
    if (!prepared->strikes) {
        return std::nullopt;
    }
    for (const auto& area : state.items) {
        const auto attack = descriptor(area.item, area.pojoTurbo);
        if (area.actor >= actors.size() || !effect(area.effect) || !attack) {
            return std::nullopt;
        }
        prepared->items.push_back({*attack, area.actor, area.effect, area.elapsed, area.lifetime,
                                   area.pojoTurbo, area.hit});
    }
    for (const auto& potion : state.potions) {
        const auto impact = restoreImpact(potion.impact, resources);
        if (!impact || (impact->owner >= 0 && !slots.contains(impact->owner))) {
            return std::nullopt;
        }
        prepared->potions.push_back(
            {*impact, potion.elapsed, potion.duration, potion.hit, potion.blessed, potion.struck});
    }
    for (const auto& entry : state.strikeEffects) {
        if (!strike(entry.strike) || !effect(entry.effect)) {
            return std::nullopt;
        }
        prepared->strikeEffects.push_back({entry.strike, entry.effect});
    }
    for (const auto& source : state.strikeSources) {
        if (source.actor >= actors.size() || !strike(source.strike) || !owner.m_resources) {
            return std::nullopt;
        }
        const auto* stats = owner.m_resources->classes.stats(actors[source.actor].character);
        if (stats == nullptr || static_cast<usize>(source.row) >= stats->moveStrikes.size()) {
            return std::nullopt;
        }
        const auto live =
            std::ranges::find(state.strikes.strikes, source.strike, &MoveStrikes::Strike::id);
        if (live != state.strikes.strikes.end() && live->owner != actors[source.actor].player) {
            return std::nullopt;
        }
        PlayerAttacks::StrikeSource copied{source.strike,      source.actor,     source.row, {},
                                           source.multiplayer, source.damageType};
        for (const auto& contact : source.contacts) {
            copied.contacts.push_back({contact.target, contact.remaining});
        }
        prepared->strikeSources.push_back(std::move(copied));
    }
    for (const auto& live : state.strikes.strikes) {
        if (!std::ranges::any_of(state.strikeSources,
                                 [&](const auto& source) { return source.strike == live.id; })) {
            return std::nullopt;
        }
    }
    for (const auto& attachment : state.attachments) {
        if (attachment.actor >= actors.size() || !effect(attachment.effect) ||
            !strike(attachment.strike)) {
            return std::nullopt;
        }
        prepared->attachments.push_back({attachment.actor, attachment.effect, attachment.strike,
                                         attachment.local, attachment.bodyParent});
    }
    for (const auto& particle : state.particles) {
        if (particle.actor >= actors.size() || !effect(particle.effect) ||
            !effect(particle.parent)) {
            return std::nullopt;
        }
        prepared->particles.push_back(
            {particle.actor, particle.effect, particle.parent, particle.node, particle.offset});
    }
    for (const auto& shield : state.shields) {
        if (shield.actor >= actors.size() || !effect(shield.effect)) {
            return std::nullopt;
        }
        PlayerAttacks::PotionShield copied{
            shield.actor,       shield.flags, shield.effect, shield.radius, shield.damage,
            shield.secondsLeft, {},           shield.blessed};
        copied.marks.reserve(shield.marks.size());
        for (const auto& mark : shield.marks) {
            copied.marks.push_back({mark.kind, mark.id, mark.wait});
        }
        prepared->shields.push_back(std::move(copied));
    }
    return Prepared{std::move(prepared)};
}
bool PlayerAttacksCheckpoint::canCommit(const PlayerAttacks& owner,
                                        const Prepared& prepared) noexcept {
    return prepared.m_data && prepared.m_data->owner == &owner &&
           !prepared.m_data->lifetime.expired() &&
           prepared.m_data->lifetime.lock() == owner.m_restoreOwner &&
           owner.m_restoreGeneration != 0 &&
           prepared.m_data->generation == owner.m_restoreGeneration && prepared.m_data->strikes &&
           owner.m_strikes.canCommit(*prepared.m_data->strikes);
}
bool PlayerAttacksCheckpoint::commit(PlayerAttacks& owner, Prepared&& prepared) noexcept {
    if (!canCommit(owner, prepared)) {
        return false;
    }
    auto state = std::move(prepared.m_data);
    if (!owner.m_strikes.commitRestore(std::move(*state->strikes))) {
        return false;
    }
    owner.m_sim.items.swap(state->items);
    owner.m_sim.potions.swap(state->potions);
    owner.m_sim.strikeEffects.swap(state->strikeEffects);
    owner.m_sim.strikeSources.swap(state->strikeSources);
    owner.m_sim.moveAttachments.swap(state->attachments);
    owner.m_sim.particleEffects.swap(state->particles);
    owner.m_sim.shields.swap(state->shields);
    ++owner.m_restoreGeneration;
    return true;
}
bool PlayerAttacksCheckpoint::restore(PlayerAttacks& owner, const State& state,
                                      std::span<const Actor> actors,
                                      const EffectTrees::PoolCheckpoint& effects,
                                      std::span<const PlayerMissiles::Resource> resources) {
    auto prepared = prepare(owner, state, actors, effects, resources);
    return prepared && commit(owner, std::move(*prepared));
}
} // namespace gdl::game
