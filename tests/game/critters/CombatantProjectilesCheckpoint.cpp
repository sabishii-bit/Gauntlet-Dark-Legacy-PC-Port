#include "game/critters/CombatantProjectilesCheckpoint.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "game/level/FixtureCheckpointDetail.h"

namespace gdl::game {
namespace {
using fixture_checkpoint::matrix;
using fixture_checkpoint::name;
using fixture_checkpoint::scalar;
using fixture_checkpoint::vector;
bool nonnegative(f32 value) {
    return scalar(value) && value >= 0;
}
bool player(s32 value) {
    return value >= -1 && value < 4;
}
bool kind(CombatantKind value) {
    return value == CombatantKind::Unknown || value == CombatantKind::General ||
           value == CombatantKind::Golem || value == CombatantKind::Gargoyle ||
           value == CombatantKind::Boss;
}
auto areaValues(const CritterArea& v) {
    return std::tie(v.local, v.node, v.worldParent, v.velocity, v.displacement, v.radius, v.minDot,
                    v.damage, v.flags, v.secondsLeft, v.lifetime, v.expanding, v.linearGrowth,
                    v.halfHeight);
}
bool areaValid(const CritterArea& v) {
    return matrix(v.local) && name(v.node) && (!v.worldParent || matrix(*v.worldParent)) &&
           vector(v.velocity) && vector(v.displacement) && nonnegative(v.radius) &&
           scalar(v.minDot) && v.minDot >= -1 && v.minDot <= 1 && nonnegative(v.damage) &&
           scalar(v.secondsLeft) && nonnegative(v.lifetime);
}
auto hitValues(const CombatantProjectileHit& v) {
    return std::tie(v.player, v.damage, v.flags, v.direction, v.repeatGap, v.critter, v.ownerKind,
                    v.target, v.ricochet, v.position, v.instance);
}
auto worldValues(const CombatantWorldHit& v) {
    return std::tie(v.position, v.object, v.splash, v.instance);
}
template <class T, class F> bool equal(const std::vector<T>& a, const std::vector<T>& b, F fields) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [&](const auto& x, const auto& y) { return fields(x) == fields(y); });
}
template <class To, class From> void flightFields(To& to, const From& from) {
    to.position = from.position;
    to.velocity = from.velocity;
    to.spin = from.spin;
    to.rotation = from.rotation;
    to.effect = from.effect;
    to.morphed = from.morphed;
    to.stuck = from.stuck;
    to.planted = from.planted;
    to.phaseSeconds = from.phaseSeconds;
    to.settled = from.settled;
    to.impactRadius = from.impactRadius;
    to.impactArea = from.impactArea;
    to.leavesGenerator = from.leavesGenerator;
    to.summonsEnemies = from.summonsEnemies;
    to.contactSeconds = from.contactSeconds;
    to.piercedPlayer = from.piercedPlayer;
    to.reflected = from.reflected;
    to.instance = from.instance;
}
template <class To, class From> void shotFields(To& to, const From& from) {
    to.critter = from.critter;
    to.damageIndex = from.damageIndex;
    to.origin = from.origin;
    to.forward = from.forward;
    to.target = from.target;
    to.rate = from.rate;
    to.scale = from.scale;
    to.damageScale = from.damageScale;
    to.birthLife = from.birthLife;
    to.endVisual = from.endVisual;
    to.realm = from.realm;
}
} // namespace

bool CombatantProjectilesCheckpoint::Flight::valid() const {
    const bool shotValid =
        shot.definition.resource != 0 && shot.definition.index < CombatantCheckpoint::kMaxParts &&
        shot.critter >= -1 && shot.damageIndex >= 0 &&
        shot.damageIndex < static_cast<s32>(CombatantCheckpoint::kMaxRows) && vector(shot.origin) &&
        vector(shot.forward) && (!shot.target || vector(*shot.target)) && nonnegative(shot.rate) &&
        nonnegative(shot.scale) && nonnegative(shot.damageScale) && scalar(shot.birthLife) &&
        shot.realm >= 'A' && shot.realm <= 'Z';
    return shotValid && archive != 0 && effect != 0 && instance != 0 && vector(position) &&
           vector(velocity) && vector(spin) && vector(rotation) && nonnegative(phaseSeconds) &&
           nonnegative(impactRadius) && (!impactArea || areaValid(*impactArea)) &&
           nonnegative(contactSeconds) && contactSeconds < 1.0f / 30 && player(piercedPlayer) &&
           (!planted || stuck) && (!impactArea || settled);
}
bool CombatantProjectilesCheckpoint::Flight::operator==(const Flight& b) const {
    return std::tie(shot, archive, position, velocity, spin, rotation, effect, morphed, stuck,
                    planted, phaseSeconds, settled, impactRadius, leavesGenerator, summonsEnemies,
                    contactSeconds, piercedPlayer, reflected, instance) ==
               std::tie(b.shot, b.archive, b.position, b.velocity, b.spin, b.rotation, b.effect,
                        b.morphed, b.stuck, b.planted, b.phaseSeconds, b.settled, b.impactRadius,
                        b.leavesGenerator, b.summonsEnemies, b.contactSeconds, b.piercedPlayer,
                        b.reflected, b.instance) &&
           impactArea.has_value() == b.impactArea.has_value() &&
           (!impactArea || areaValues(*impactArea) == areaValues(*b.impactArea));
}
bool CombatantProjectilesCheckpoint::State::valid() const {
    if (catalog.size() > kMaxResources || flying.size() > kMaxFlights ||
        emittedEffects.size() > kMaxOutputs || hits.size() > kMaxOutputs ||
        worldHits.size() > kMaxOutputs || rockHits.size() > kMaxOutputs ||
        generators.size() > kMaxOutputs || summons.size() > kMaxOutputs ||
        !nonnegative(ricochetIn) || ricochetIn > 1 || !random.valid()) {
        return false;
    }
    for (usize i = 0; i < catalog.size(); ++i) {
        const auto& row = catalog[i];
        if (row.id == 0 || (i > 0 && catalog[i - 1].id >= row.id) || !kind(row.kind) ||
            !name(row.name) || row.name.empty() ||
            row.children.size() >= CombatantCheckpoint::kMaxParts ||
            !std::ranges::all_of(row.children,
                                 [](const auto& child) { return name(child) && !child.empty(); })) {
            return false;
        }
    }
    std::unordered_set<u32> flightEffects;
    std::unordered_set<u64> instances;
    for (const auto& flight : flying) {
        const auto definition =
            std::ranges::find(catalog, flight.shot.definition.resource, &Catalog::id);
        if (!flight.valid() || !flightEffects.insert(flight.effect).second ||
            (nextInstance != 0 && flight.instance >= nextInstance) ||
            !instances.insert(flight.instance).second || definition == catalog.end() ||
            flight.shot.definition.index > definition->children.size() ||
            std::ranges::find(catalog, flight.archive, &Catalog::id) == catalog.end()) {
            return false;
        }
    }
    // show() appends freshly issued native IDs; pruning preserves that order.
    for (usize index = 0; index < emittedEffects.size(); ++index) {
        if (emittedEffects[index] == 0 ||
            (index > 0 && emittedEffects[index - 1] >= emittedEffects[index])) {
            return false;
        }
    }
    return std::ranges::all_of(hits,
                               [&](const auto& hit) {
                                   return player(hit.player) && nonnegative(hit.damage) &&
                                          vector(hit.direction) && nonnegative(hit.repeatGap) &&
                                          hit.critter >= -1 && kind(hit.ownerKind) &&
                                          hit.target >= -1 && vector(hit.position) &&
                                          (nextInstance == 0 || hit.instance < nextInstance);
                               }) &&
           std::ranges::all_of(worldHits,
                               [&](const auto& hit) {
                                   return vector(hit.position) && hit.object >= -1 &&
                                          (nextInstance == 0 || hit.instance < nextInstance);
                               }) &&
           std::ranges::all_of(
               rockHits,
               [](const auto& hit) { return hit.rock <= 65535 && nonnegative(hit.damage); }) &&
           std::ranges::all_of(generators, matrix) && std::ranges::all_of(summons, matrix);
}
bool CombatantProjectilesCheckpoint::State::operator==(const State& b) const {
    return catalog == b.catalog && flying == b.flying && emittedEffects == b.emittedEffects &&
           equal(hits, b.hits, hitValues) && equal(worldHits, b.worldHits, worldValues) &&
           rockHits == b.rockHits && generators == b.generators && summons == b.summons &&
           random == b.random && ricochetIn == b.ricochetIn && nextInstance == b.nextInstance;
}
struct CombatantProjectilesCheckpoint::Prepared::Data {
    const CombatantProjectilesCheckpoint* wrapper = nullptr;
    const CombatantProjectiles* owner = nullptr;
    std::shared_ptr<const u8> binding, lifetime, nextBinding;
    u64 revision = 0;
    std::vector<CombatantProjectiles::Flying> flying;
    State state;
};
CombatantProjectilesCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
CombatantProjectilesCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
CombatantProjectilesCheckpoint::Prepared&
CombatantProjectilesCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
CombatantProjectilesCheckpoint::Prepared::~Prepared() = default;

CombatantProjectilesCheckpoint::CombatantProjectilesCheckpoint(
    CombatantProjectiles& owner, EffectTrees& effects, std::span<const Resource> resources,
    std::span<const EffectTrees::PoolResource> effectResources)
    : m_owner(owner), m_effects(effects), m_resources(resources.begin(), resources.end()),
      m_effectResources(effectResources.begin(), effectResources.end()) {
    if (m_resources.size() > kMaxResources) {
        return;
    }
    std::ranges::sort(m_resources, {}, &Resource::id);
    for (const auto& r : m_resources) {
        if (r.stock == nullptr) {
            return;
        }
        m_resourceBindings.push_back(r.stock->m_checkpointBinding);
        m_children.push_back(r.stock->children.data());
        Catalog row{r.id, r.stock->data.kind(), std::string{r.stock->data.name()}, {}};
        for (const auto& child : r.stock->children) {
            row.children.emplace_back(child.name());
        }
        m_catalog.push_back(std::move(row));
    }
}
CombatantProjectilesCheckpoint::~CombatantProjectilesCheckpoint() = default;
bool CombatantProjectilesCheckpoint::bound() const {
    if (m_resources.size() > kMaxResources || m_catalog.size() != m_resources.size() ||
        m_resourceBindings.size() != m_resources.size() ||
        m_children.size() != m_resources.size() ||
        m_effectResources.size() >
            EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks) {
        return false;
    }
    for (usize i = 0; i < m_resources.size(); ++i) {
        const auto& r = m_resources[i];
        if (r.id == 0 || r.stock == nullptr ||
            r.stock->m_checkpointBinding != m_resourceBindings[i] || !r.stock->data.loaded() ||
            !r.stock->archive.loaded() || (i > 0 && m_resources[i - 1].id >= r.id) ||
            r.stock->children.data() != m_children[i] ||
            r.stock->children.size() != m_catalog[i].children.size() ||
            r.stock->data.name() != m_catalog[i].name ||
            r.stock->data.kind() != m_catalog[i].kind) {
            return false;
        }
        for (usize child = 0; child < r.stock->children.size(); ++child) {
            if (r.stock->children[child].name() != m_catalog[i].children[child]) {
                return false;
            }
        }
        for (usize earlier = 0; earlier < i; ++earlier) {
            if (m_resources[earlier].stock == r.stock) {
                return false; // One stable identity per immutable stock.
            }
        }
    }
    return true;
}
const CombatantProjectilesCheckpoint::Resource*
CombatantProjectilesCheckpoint::resource(u32 id) const {
    const auto found = std::ranges::find(m_resources, id, &Resource::id);
    return found == m_resources.end() ? nullptr : &*found;
}
std::optional<CombatantProjectilesCheckpoint::Definition>
CombatantProjectilesCheckpoint::definition(const CritterData* value) const {
    for (const auto& r : m_resources) {
        if (value == &r.stock->data) {
            return Definition{r.id, 0};
        }
        for (usize i = 0; i < r.stock->children.size(); ++i) {
            if (value == &r.stock->children[i]) {
                return Definition{r.id, static_cast<u32>(i + 1)};
            }
        }
    }
    return std::nullopt;
}
const CritterData* CombatantProjectilesCheckpoint::definition(Definition value) const {
    const auto* r = resource(value.resource);
    if (r == nullptr || value.index > r->stock->children.size()) {
        return nullptr;
    }
    return value.index == 0 ? &r->stock->data : &r->stock->children[value.index - 1];
}
bool CombatantProjectilesCheckpoint::references(const State& state,
                                                const EffectTrees::PoolCheckpoint& effects) const {
    if (!state.valid() || !effects.valid() || state.catalog != m_catalog) {
        return false;
    }
    const auto issued = [&](u32 id) {
        return id != 0 && (effects.nextId == 0 || id < effects.nextId);
    };
    if (!std::ranges::all_of(state.emittedEffects, issued)) {
        return false;
    }
    for (const auto& flight : state.flying) {
        const auto* data = definition(flight.shot.definition);
        const auto* archive = resource(flight.archive);
        if (data == nullptr || archive == nullptr || !issued(flight.effect) ||
            data->damage(flight.shot.damageIndex) == nullptr) {
            return false;
        }
        const auto& damage = *data->damage(flight.shot.damageIndex);
        constexpr u32 kSticky = 0x4000000;
        const bool planted =
            damage.type == AttackDefinition::kTargetArea && (damage.flags & kSticky) != 0;
        if (flight.planted != planted ||
            (!planted && damage.type != AttackDefinition::kProjectile)) {
            return false;
        }
        const auto effect = std::ranges::find(effects.effects, flight.effect,
                                              &EffectTrees::PoolCheckpoint::Entry::id);
        if (effect != effects.effects.end()) {
            const auto binding = std::ranges::find(m_effectResources, effect->resource,
                                                   &EffectTrees::PoolResource::id);
            s32 cueIndex = damage.sound;
            if (!planted && (flight.stuck || flight.settled)) {
                cueIndex = damage.hitSound;
            } else if (flight.morphed) {
                cueIndex = damage.morph;
            }
            const auto* cue = data->sound(cueIndex);
            if (binding == m_effectResources.end() ||
                binding->archive != &archive->stock->archive || !effect->state.tree ||
                cue == nullptr || effect->state.name != cue->tree) {
                return false;
            }
        }
        // Missing effects are legitimate after effects.update/stop but before
        // this pool observes expiry. Preserve the pending native morph/cleanup.
    }
    return true;
}
std::optional<CombatantProjectilesCheckpoint::State>
CombatantProjectilesCheckpoint::checkpoint() const {
    const auto effects = m_effects.checkpointPool(m_effectResources);
    return effects ? checkpoint(*effects) : std::nullopt;
}
std::optional<CombatantProjectilesCheckpoint::State>
CombatantProjectilesCheckpoint::checkpoint(const EffectTrees::PoolCheckpoint& effects) const {
    if (!bound() || m_owner.m_sim.flying.size() > kMaxFlights ||
        m_owner.m_sim.emittedEffects.size() > kMaxOutputs ||
        m_owner.m_sim.hits.size() > kMaxOutputs || m_owner.m_sim.worldHits.size() > kMaxOutputs ||
        m_owner.m_sim.rockHits.size() > kMaxOutputs ||
        m_owner.m_sim.generators.size() > kMaxOutputs ||
        m_owner.m_sim.summons.size() > kMaxOutputs) {
        return std::nullopt;
    }
    State state;
    state.catalog = m_catalog;
    for (const auto& native : m_owner.m_sim.flying) {
        const auto data = definition(native.shot.data);
        const auto archive = std::ranges::find_if(
            m_resources, [&](const auto& r) { return &r.stock->archive == native.archive; });
        if (!data || archive == m_resources.end()) {
            return std::nullopt;
        }
        Flight flight;
        flightFields(flight, native);
        shotFields(flight.shot, native.shot);
        flight.shot.definition = *data;
        flight.archive = archive->id;
        state.flying.push_back(std::move(flight));
    }
    state.emittedEffects = m_owner.m_sim.emittedEffects;
    state.hits = m_owner.m_sim.hits;
    state.worldHits = m_owner.m_sim.worldHits;
    state.rockHits = m_owner.m_sim.rockHits;
    state.generators = m_owner.m_sim.generators;
    state.summons = m_owner.m_sim.summons;
    state.random = m_owner.m_sim.random.checkpoint();
    state.ricochetIn = m_owner.m_sim.ricochetIn;
    state.nextInstance = m_owner.m_sim.nextInstance;
    return references(state, effects) ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<CombatantProjectilesCheckpoint::Prepared>
CombatantProjectilesCheckpoint::prepareRestore(const State& state,
                                               const EffectTrees::PoolCheckpoint& effects) const {
    if (!bound() || !references(state, effects) ||
        !m_effects.prepareRestorePool(effects, m_effectResources)) {
        return std::nullopt;
    }
    auto next = std::make_unique<Prepared::Data>();
    next->wrapper = this;
    next->owner = &m_owner;
    next->binding = m_owner.m_checkpointBinding;
    next->revision = m_owner.m_checkpointRevision;
    next->lifetime = m_lifetime;
    next->nextBinding = std::make_shared<const u8>(0);
    next->state = state;
    next->flying.reserve(state.flying.size());
    for (const auto& flight : state.flying) {
        CombatantProjectiles::Flying native;
        flightFields(native, flight);
        shotFields(native.shot, flight.shot);
        native.shot.data = definition(flight.shot.definition);
        native.archive = &resource(flight.archive)->stock->archive;
        next->flying.push_back(std::move(native));
    }
    return Prepared{std::move(next)};
}
bool CombatantProjectilesCheckpoint::canCommit(const Prepared& prepared) const {
    return prepared.m_data && bound() && prepared.m_data->wrapper == this &&
           prepared.m_data->owner == &m_owner &&
           prepared.m_data->binding == m_owner.m_checkpointBinding &&
           prepared.m_data->revision == m_owner.m_checkpointRevision &&
           prepared.m_data->lifetime == m_lifetime;
}
bool CombatantProjectilesCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto next = std::move(prepared.m_data);
    m_owner.m_sim.flying.swap(next->flying);
    m_owner.m_sim.emittedEffects.swap(next->state.emittedEffects);
    m_owner.m_sim.hits.swap(next->state.hits);
    m_owner.m_sim.worldHits.swap(next->state.worldHits);
    m_owner.m_sim.rockHits.swap(next->state.rockHits);
    m_owner.m_sim.generators.swap(next->state.generators);
    m_owner.m_sim.summons.swap(next->state.summons);
    (void)m_owner.m_sim.random.restore(next->state.random); // Validated before mutation.
    m_owner.m_sim.ricochetIn = next->state.ricochetIn;
    m_owner.m_sim.nextInstance = next->state.nextInstance;
    m_owner.m_checkpointBinding = std::move(next->nextBinding);
    ++m_owner.m_localEpoch;
    return true;
}
bool CombatantProjectilesCheckpoint::restore(const State& state,
                                             const EffectTrees::PoolCheckpoint& effects) {
    auto prepared = prepareRestore(state, effects);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
