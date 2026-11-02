#include "game/enemies/CombatResources.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "game/enemies/LevelOpponents.h"
#include "game/party/TurboEmission.h"

namespace gdl::game {
namespace {
constexpr u32 kBankLimit = 0x8000;
constexpr u32 kTreeLimit = 16383;
constexpr usize kResourceLimit =
    EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks;
constexpr u32 kParticleBit = 0x80000000U;

bool sameBank(const CombatResources::Bank& left, const CombatResources::Bank& right) {
    return left.archive == right.archive && left.lenders == right.lenders;
}
bool sameMissile(const PlayerMissiles::Resource& left, const PlayerMissiles::Resource& right) {
    return left.spec == right.spec && left.model == right.model && left.streak == right.streak;
}
} // namespace

struct CombatResources::Data {
    struct BoundBank {
        Bank binding;
        TextureAnimator textures;
        std::vector<std::unique_ptr<EffectTrees::Effect>> effects;
    };
    struct Player {
        s32 seat = -1;
        s32 character = -1;
        const PlayerFigure* figure = nullptr;
    };
    std::vector<std::unique_ptr<BoundBank>> banks;
    std::vector<EffectTrees::PoolResource> effects;
    std::vector<PlayerMissiles::Resource> missiles;
    std::vector<Player> players;
    bool stage = false;

    bool addEffect(BoundBank& bank, u32 id, const EffectTrees::Effect& prototype) {
        if (effects.size() >= kResourceLimit) {
            return false;
        }
        bank.effects.push_back(std::make_unique<EffectTrees::Effect>(prototype));
        effects.push_back({id, bank.binding.archive, bank.binding.lenders, &bank.textures,
                           bank.effects.back().get()});
        return true;
    }
};

CombatResources::CombatResources() = default;
CombatResources::~CombatResources() = default;
void CombatResources::clear() {
    m_data.reset();
}

bool CombatResources::bind(RenderDevice& device, std::span<const Bank> banks,
                           std::span<const PlayerMissiles::Resource> missiles,
                           std::span<const Particle> particles) {
    if (banks.size() > EffectTrees::PoolCheckpoint::kMaxBanks ||
        missiles.size() > PlayerMissiles::Checkpoint::kMaxResources ||
        particles.size() > EffectTrees::PoolCheckpoint::kMaxEffects) {
        return false;
    }
    auto next = std::make_unique<Data>();
    std::vector<Bank> ordered{banks.begin(), banks.end()};
    std::ranges::sort(ordered, {}, &Bank::id);
    std::map<u32, Data::BoundBank*> known;
    std::set<u32> ids;
    for (const auto& bank : ordered) {
        if (bank.id == 0 || bank.id >= kBankLimit || !ids.insert(bank.id).second ||
            bank.archive == nullptr || !bank.archive->loaded() ||
            bank.archive->trees.size() > kTreeLimit || bank.lenders.size() > 64 ||
            std::ranges::any_of(bank.lenders, [](const auto* value) { return value == nullptr; })) {
            return false;
        }
        const auto alias = std::ranges::find_if(
            next->banks, [&](const auto& value) { return sameBank(value->binding, bank); });
        if (alias != next->banks.end()) {
            known.emplace(bank.id, alias->get());
            continue;
        }
        auto prepared = std::make_unique<Data::BoundBank>();
        prepared->binding = bank;
        prepared->textures.bind(bank.archive->trees.textureAnimations(), bank.archive->textures,
                                device, bank.lenders);
        known.emplace(bank.id, prepared.get());
        next->banks.push_back(std::move(prepared));
    }
    // Bank-only entries precede every effect. Their identity never changes when
    // the first tree starts, the last one dies, or live effects reorder.
    for (const auto& bank : next->banks) {
        next->effects.push_back({bank->binding.id << 16, bank->binding.archive,
                                 bank->binding.lenders, &bank->textures, nullptr});
    }
    for (const auto& bank : next->banks) {
        EffectTrees prototypes;
        auto& archive = *bank->binding.archive;
        for (u32 index = 0; index < archive.trees.size(); ++index) {
            const auto& tree = archive.trees.tree(index);
            // The only native sparse-object interpretation in this domain is
            // the elemental WEAP_TW rider; ordinary trees retain strict binding.
            for (const bool emits : {true, false}) {
                EffectTrees::Setting setting;
                setting.emitParticles = emits;
                setting.missingObjectsAreEmpty = tree.name.starts_with("WEAP_TW_");
                const u32 local = prototypes.startSet(device, archive, tree.name, {}, setting,
                                                      bank->binding.lenders);
                if (local == 0) {
                    // Such a tree cannot start with this exact native context.
                    // It is not replaced with a guessed model or particle set.
                    continue;
                }
                const u32 variant = (setting.missingObjectsAreEmpty ? 2U : 0U) + (emits ? 1U : 2U);
                const u32 id = (bank->binding.id << 16) | (index * 4 + variant);
                if (!next->addEffect(*bank, id, prototypes.effect(0))) {
                    return false;
                }
                prototypes.stop(local);
            }
        }
    }
    ids.clear();
    for (const auto& particle : particles) {
        const auto found = known.find(particle.bank);
        if (particle.id == 0 || (particle.id & kParticleBit) != 0 ||
            !ids.insert(particle.id).second || found == known.end()) {
            return false;
        }
        auto& bank = *found->second;
        auto& archive = *bank.binding.archive;
        if (!bank.binding.lenders.empty() || particle.texture >= archive.textures.size() ||
            archive.textures.entry(particle.texture).external() ||
            archive.textures.entry(particle.texture).noPicture) {
            return false;
        }
        ParticleEmitter check;
        check.start(particle.descriptor, Mat4{1});
        if (!check.checkpoint().valid()) {
            return false;
        }
        EffectTrees prototypes;
        const u32 local = prototypes.startParticles(device, archive, particle.descriptor,
                                                    particle.texture, Mat4{1});
        if (local == 0 ||
            !next->addEffect(bank, kParticleBit | particle.id, prototypes.effect(0))) {
            return false;
        }
    }
    ids.clear();
    next->missiles.assign(missiles.begin(), missiles.end());
    std::ranges::sort(next->missiles, {}, &PlayerMissiles::Resource::id);
    std::vector<PlayerMissiles::Resource> unique;
    for (const auto& missile : next->missiles) {
        if (missile.id == 0 || !ids.insert(missile.id).second || missile.spec == nullptr) {
            return false;
        }
        if (std::ranges::none_of(unique,
                                 [&](const auto& value) { return sameMissile(value, missile); })) {
            unique.push_back(missile);
        }
    }
    next->missiles = std::move(unique);
    // Reuse the native resource validators; an empty pool performs no update,
    // resolves no artwork, and consumes no caller-owned outputs.
    const PlayerMissiles empty;
    if (!empty.checkpoint(next->missiles) || !EffectTrees{}.checkpointPool(next->effects)) {
        return false;
    }
    m_data = std::move(next);
    return true;
}

bool CombatResources::bindStage(RenderDevice& device, LevelWorld& world, ItemArchive& weapons,
                                const ClassDataSet& classes, std::span<PlayerRuntime> party,
                                const PlayerArsenal& arsenal, LevelOpponents& opponents,
                                const EffectTrees& effects) {
    if (party.empty() || party.size() > 4 || !weapons.loaded()) {
        return false;
    }
    std::vector<Bank> banks;
    std::vector<Particle> particles;
    std::vector<PlayerMissiles::Resource> missiles;
    std::vector<Data::Player> players;
    const auto shared = effects.textureLenders();
    const auto thrown = arsenal.missiles().visuals().textureLenders();
    const auto add = [&](u32 id, ItemArchive* archive, std::span<TextureSet* const> lenders) {
        if (archive != nullptr && archive->loaded()) {
            banks.push_back({id, archive, {lenders.begin(), lenders.end()}});
        }
    };
    add(1, &weapons, shared);
    add(2, &world.powerups(), shared);
    add(3, &world.items(), shared);
    add(4, &world.realmItems(), shared);
    add(5, &weapons, {}); // Native standalone SFXX does not use scene lenders.
    add(6, &weapons, thrown);
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        add(32 + static_cast<u32>(kind), opponents.enemies().archive(kind), shared);
    }
    auto stocks = opponents.critters().resources();
    for (auto* stock : opponents.bosses().resources()) {
        stocks.push_back(stock);
    }
    std::ranges::sort(stocks, {}, [](const auto* stock) { return stock->definition.name; });
    if (stocks.size() > 16) {
        return false;
    }
    for (usize index = 0; index < stocks.size(); ++index) {
        if (index != 0 && stocks[index - 1]->definition.name == stocks[index]->definition.name) {
            return false;
        }
        add(128 + static_cast<u32>(index), &stocks[index]->archive, shared);
    }
    const Texture* streak = nullptr;
    if (const auto slot = weapons.textures.find("WEP_STREAK")) {
        streak = &weapons.textures.texture(device, *slot);
    }
    std::set<s32> seats;
    for (auto& player : party) {
        const auto seat = player.actor.player();
        const auto character = player.actor.save().character;
        const auto* stats = classes.stats(character);
        if (seat < 0 || seat >= 4 || !seats.insert(seat).second || !player.figure ||
            stats == nullptr || stats->moveEffects.size() > 1024) {
            return false;
        }
        auto& figure = *player.figure;
        auto* classEffects = figure.effects();
        auto* weaponArchive = figure.missileArchive();
        if (classEffects == nullptr || weaponArchive == nullptr) {
            return false;
        }
        const u32 base = 256 + static_cast<u32>(seat) * 8;
        std::vector<TextureSet*> weaponLenders{&weapons.textures};
        weaponLenders.insert(weaponLenders.end(), thrown.begin(), thrown.end());
        add(base, classEffects, shared);
        add(base + 1, classEffects, weaponLenders);
        add(base + 2, weaponArchive, weaponLenders);
        add(base + 4, classEffects, {});
        add(base + 5, classEffects, thrown); // Native earned familiar has no primary lender.
        std::vector<TextureSet*> gauntletLenders{&classEffects->textures};
        gauntletLenders.insert(gauntletLenders.end(), thrown.begin(), thrown.end());
        add(base + 3, &weapons, gauntletLenders);
        players.push_back({seat, character, &figure});
        const u32 shot = 1 + static_cast<u32>(seat) * 4;
        missiles.push_back({shot, &MissileSpec::of(character), &figure.missile(), streak});
        missiles.push_back({shot + 1, &MissileSpec::of(character), nullptr, streak});
        const bool boss = world.level() != nullptr && world.level()->bossType >= 0;
        const auto* familiar = &PlayerArsenal::familiarSpec(boss);
        if (figure.familiarMissile().bound()) {
            missiles.push_back({shot + 2, familiar, &figure.familiarMissile(), nullptr});
        }
        if (arsenal.phoenixMissile().bound()) {
            missiles.push_back({shot + 3, familiar, &arsenal.phoenixMissile(), nullptr});
        }
        for (usize row = 0; row < stats->moveEffects.size(); ++row) {
            const auto& authored = stats->moveEffects[row];
            if (!authored.particle()) {
                continue;
            }
            for (const auto [archive, bank] :
                 {std::pair{classEffects, base + 4}, std::pair{&weapons, u32{5}}}) {
                const auto texture = archive->textures.find(authored.tree);
                if (texture && !archive->textures.entry(*texture).external() &&
                    !archive->textures.entry(*texture).noPicture) {
                    particles.push_back({1 + static_cast<u32>(seat) * 1024 + static_cast<u32>(row),
                                         bank, TurboEmission::particle(authored).descriptor,
                                         *texture});
                    break;
                }
            }
        }
    }
    for (s32 kind = 0; kind <= 4; ++kind) {
        if (const auto* model = arsenal.potionModel(kind)) {
            missiles.push_back(
                {128 + static_cast<u32>(kind), &MissileSpec::potion(), model, nullptr});
        }
    }
    if (!bind(device, banks, missiles, particles)) {
        return false;
    }
    m_data->players = std::move(players);
    m_data->stage = true;
    return true;
}

CombatRollbackCheckpoint::Resources CombatResources::resources() const {
    if (!m_data) {
        return {};
    }
    return {m_data->missiles, m_data->effects, m_data->effects};
}
bool CombatResources::matches(std::span<const PlayerRuntime> party) const {
    if (!m_data || !m_data->stage || party.size() != m_data->players.size()) {
        return false;
    }
    u32 seats = 0;
    for (const auto& player : party) {
        const auto seat = player.actor.player();
        if (seat < 0 || seat >= 4 || (seats & (1U << seat)) != 0) {
            return false;
        }
        seats |= 1U << seat;
        const auto found = std::ranges::find(m_data->players, seat, &Data::Player::seat);
        if (found == m_data->players.end() || found->character != player.actor.save().character ||
            found->figure != player.figure.get()) {
            return false;
        }
    }
    return true;
}
usize CombatResources::bankCount() const {
    return m_data ? m_data->banks.size() : 0;
}
usize CombatResources::effectCount() const {
    return m_data ? m_data->effects.size() - m_data->banks.size() : 0;
}
} // namespace gdl::game
