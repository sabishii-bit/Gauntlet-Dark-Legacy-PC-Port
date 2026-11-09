#include "game/enemies/Generators.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ObjectFlags.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/enemies/EnemyKinds.h"
#include "game/level/ItemSupport.h"
#include "game/level/NativeStateResources.h"
#include "game/party/TargetAssist.h"

namespace gdl::game {

namespace {
/** lbl_8011BB20: a hit, by the kind bred. */
constexpr std::array<s32, static_cast<usize>(kEnemyKindCount)> kHitWorth{
    1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 2, 1, 2,
    3, 1, 2, 3, 1, 1, 2, 3, 3, 3, 1, 0, 2, 1, 1, 2, 2};
constexpr s32 kExperienceShare = 5;

void appendSlot(NativeStateDigest& digest, const Generators::Slot& slot) {
    // All native Slot members are decomposed, including hidden and dead slots.
    const auto append = [&]<class T>(const T& field) {
        if constexpr (std::is_same_v<T, Obstacle>) {
            const auto& [centre, yaw, across, along, height, solid, radius, item] = field;
            digest.values(centre, yaw, across, along, height, solid, radius);
            digest.flag(item.has_value());
            if (item) {
                digest.fields<2>(*item);
            }
        } else {
            digest.value(field);
        }
    };
    visitNativeFields<32>(slot, [&](const auto&... fields) { (append(fields), ...); });
}

// C++ cannot decompose a record with both base and derived data members. The
// actual Slot base is visited above; this detects another derived member before
// the explicit two-member extension below can silently omit it.
template <class T>
constexpr bool kHasExtraGeneratorMember =
    requires { T{Generators::Slot{}, std::optional<ItemFigure>{}, u64{}, {}}; };
} // namespace

void Generators::appendSimulationDigest(
    NativeStateDigest& digest, const Simulation& state,
    std::optional<std::span<const TextureAnimator::Clock>> savedClocks) {
    const auto& [collision, generators, view, attentionGate, bodies, scales, players, specialBirth,
                 nextInstance, bound, importedRandom] = state;
    digest.resource(collision);
    digest.wide(generators.size());
    static_assert(std::is_aggregate_v<Generator>);
    static_assert(requires { Generator{Slot{}, std::optional<ItemFigure>{}, u64{}}; });
    static_assert(!kHasExtraGeneratorMember<Generator>);
    for (const auto& generator : generators) {
        appendSlot(digest, generator);
        digest.values(generator.bossFigure, generator.instance);
    }
    digest.flag(view.has_value());
    if (view) {
        digest.value(*view);
    }
    digest.flag(attentionGate);
    digest.wide(bodies.size());
    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): every body is checked before use
    for (usize index = 0; index < bodies.size(); ++index) {
        if (bodies[index] == nullptr) {
            digest.invalidate();
            return;
        }
        const auto& [kind, archive, trees, models, textures] = *bodies[index];
        digest.value(kind);
        digest.resource(archive);
        for (usize body = 0; body < trees.size(); ++body) {
            digest.resource(&trees[body]);
            // GPU/model draw caches are not native inputs. Their immutable tree
            // and source archive identify the artwork; availability is retained.
            digest.flag(models[body].bound());
        }
        digest.resource(&textures);
        digest.fields<3>(savedClocks ? (*savedClocks)[index] : textures.clock());
    }
    digest.fields<3>(scales);
    digest.values(players, specialBirth, nextInstance, bound);
    if (importedRandom) {
        digest.string("Generators.importedRandom.1");
        digest.fields<2>(importedRandom->checkpoint());
    }
}

bool Generators::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& binding : m_bodyCache) {
        const auto* body = binding.get();
        if (body == nullptr || body->archive == nullptr) {
            return false;
        }
        for (usize state = 0; state < body->trees.size(); ++state) {
            if (!resources.registerGeneratedTree(
                    body->trees[state], *body->archive,
                    std::format("generator/{}/state/{}", body->kind, state))) {
                return false;
            }
        }
        if (!resources.registerTextureAnimator(body->textures, *body->archive,
                                               std::format("generator/{}/textures", body->kind))) {
            return false;
        }
    }
    for (const auto& generator : m_sim.generators) {
        if (generator.bossFigure && !generator.bossFigure->registerNativeResources(resources)) {
            return false;
        }
    }
    // Speculative births can leave cached artwork reachable from retained
    // history after the active generator slot is rolled back away.
    for (const auto& prototype : m_bossPrototypes) {
        if (!prototype.figure.registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

bool Generators::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("Generators.native.1");
    digest.wide(m_roster.size());
    for (const auto& slot : m_roster) {
        appendSlot(digest, slot);
    }
    appendSimulationDigest(digest, m_sim);
    appendImportedHealth(digest);
    return digest.good();
}

bool Generators::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("Generators.native.1");
    digest.wide(m_roster.size());
    for (const auto& slot : m_roster) {
        appendSlot(digest, slot);
    }
    appendSimulationDigest(digest, saved.m_state,
                           std::span<const TextureAnimator::Clock>{saved.m_clocks});
    appendImportedHealth(digest);
    return digest.good();
}

void Generators::appendImportedHealth(NativeStateDigest& digest) const {
    // Base-game histories retain their existing schema. Imported levels are
    // offline, but their native digest must still identify the damage rules.
    if (!m_importedHealth.empty()) {
        digest.string("Generators.importedHealth.1");
        digest.wide(m_importedHealth.size());
        for (const auto& health : m_importedHealth) {
            digest.values(health.instance, health.remaining);
            if (health.broodTier != 0) {
                digest.string("Generators.importedBrood.2");
                digest.values(health.broodKinds, health.broodAlgorithms, health.broodTier);
            }
            if (health.breeding) {
                digest.string("Generators.importedBreeding.1");
                digest.values(health.breeding->broodByState, health.breeding->waitTicks,
                              health.breeding->activeDistance);
            }
        }
    }
}

s32 generatorExperience(s32 kind, bool destroyed) {
    if (kind == Generators::kTempleBrood) {
        kind = 1;
    } else if (kind == Generators::kHellBrood) {
        kind = 2;
    } else if (kind < 0) {
        kind = 0;
    }
    if (kind >= kEnemyKindCount) {
        return 0;
    }
    // The destroying blow is worth the bred kind's own hit experience: lbl_8011BBA8 holds the
    // same values.
    const s32 worth =
        destroyed ? enemyKind(kind).experienceHit : kHitWorth[static_cast<usize>(kind)];
    return kExperienceShare * worth;
}

namespace {

constexpr std::array<s32, 4> kTempleKinds{enemy_type::kIce, enemy_type::kImp, enemy_type::kPlague,
                                          enemy_type::kZombie};
constexpr std::array<s32, 4> kHellKinds{enemy_type::kDemon, enemy_type::kWarlock,
                                        enemy_type::kGhost, enemy_type::kSky};
/** The strength a mixed brood breeds at: the temple's at two, the underworld's at three. */
constexpr s32 kTempleBroodStrength = 2;
constexpr s32 kHellBroodStrength = 3;
constexpr std::string_view kSpecialRecord = "SPECIAL"; ///< the broods' generator record
constexpr std::array<s32, 4> kHellAlgorithms{30, 30, 7, 7};
// A generator's kind resolves as though bred at no strength: never the medium's second row.
constexpr s32 kGeneratorKindStrength = 0;
constexpr std::array<s32, 3> kCasterAlgorithms{28, 29, 30}; ///< dropped once a state crumbles
constexpr f32 kGeneratorViewScale = 8.0f;
/** What a generator the level has no record for measures, for the developer console. */
constexpr f32 kStockGeneratorRadius = 3.0f;
constexpr f32 kStockGeneratorHeight = 6.0f;
constexpr s32 kStockGeneratorHitPoints = 100;
constexpr f32 kAttentionRange = 50.0f; ///< do_items, lbl_80343C50 (50 world units)

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool sameObstacle(const Obstacle& a, const Obstacle& b) {
    return a.centre == b.centre && a.yaw == b.yaw && a.halfAcross == b.halfAcross &&
           a.halfAlong == b.halfAlong && a.height == b.height && a.solid == b.solid &&
           a.cylinderRadius == b.cylinderRadius &&
           a.enemyItem.has_value() == b.enemyItem.has_value() &&
           (!a.enemyItem || (a.enemyItem->cylindrical == b.enemyItem->cylindrical &&
                             a.enemyItem->radius == b.enemyItem->radius));
}

bool sameView(const std::optional<ViewSet>& a, const std::optional<ViewSet>& b) {
    return a == b;
}

bool generatorKind(s32 kind) {
    return kind == Generators::kHellBrood || kind == Generators::kTempleBrood ||
           (kind >= 0 && kind < kEnemyKindCount);
}

} // namespace

void Generators::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_clocks.resize(m_sim.bodies.size());
    for (usize i = 0; i < m_sim.bodies.size(); ++i) {
        saved.m_clocks[i] = m_sim.bodies[i]->textures.clock();
    }
    saved.m_owner = m_localBinding;
    saved.m_epoch = m_localEpoch;
}

bool Generators::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner.lock() == m_localBinding && saved.m_epoch == m_localEpoch &&
           saved.m_clocks.size() == saved.m_state.bodies.size();
}

bool Generators::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    if (!canRestoreLocal(left) || !canRestoreLocal(right)) {
        return false;
    }
    const auto& a = left.m_state;
    const auto& b = right.m_state;
    if (a.collision != b.collision || a.bodies != b.bodies || left.m_clocks != right.m_clocks ||
        !sameView(a.view, b.view)) {
        return false;
    }
    const auto values = [](const Simulation& value) {
        return std::tie(value.attentionGate, value.scales, value.players, value.specialBirth,
                        value.nextInstance, value.bound, value.importedRandom);
    };
    return values(a) == values(b) &&
           std::ranges::equal(
               a.generators, b.generators, [](const Generator& first, const Generator& second) {
                   return static_cast<const Slot&>(first) == static_cast<const Slot&>(second) &&
                          first.instance == second.instance &&
                          first.bossFigure.has_value() == second.bossFigure.has_value() &&
                          (!first.bossFigure || first.bossFigure->equalLocal(*second.bossFigure));
               });
}

bool Generators::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    for (usize i = 0; i < m_sim.bodies.size(); ++i) {
        m_sim.bodies[i]->textures.restoreClock(saved.m_clocks[i]);
    }
    return true;
}

bool Generators::Slot::operator==(const Slot& other) const {
    const auto values = [](const Slot& s) {
        return std::tie(s.minPlayers, s.presence, s.kind, s.tier, s.algorithm, s.most, s.interval,
                        s.health, s.threshold, s.armor, s.state, s.countdown, s.ratio, s.bred,
                        s.living, s.position, s.collisionOffset, s.targetRadius, s.targetHeight,
                        s.placement, s.yaw, s.direction, s.clearance, s.patrolClearance,
                        s.viewRadius, s.alwaysActive, s.support, s.supportLocal, s.boxOffset,
                        s.boxYaw, s.boss);
    };
    return values(*this) == values(other) && sameObstacle(box, other.box);
}

s32 Generators::paramOf(const ItemInstance& instance, usize index) {
    if (index * 2 + 1 >= instance.params.size()) {
        return 0;
    }
    s16 value = 0;
    std::memcpy(&value, &instance.params[index * 2], sizeof(value));
    return value;
}

Generators::Bodies* Generators::bodiesOf(s32 kind) {
    for (auto& bodies : m_sim.bodies) {
        if (bodies->kind == kind) {
            return bodies;
        }
    }
    return nullptr;
}

const Generators::Bodies* Generators::bodiesOf(s32 kind) const {
    for (const auto& bodies : m_sim.bodies) {
        if (bodies->kind == kind) {
            return bodies;
        }
    }
    return nullptr;
}

bool Generators::loadBodies(RenderDevice& device, Enemies& enemies, s32 kind,
                            ItemArchive* realmItems, std::span<TextureSet* const> lenders) {
    if (bodiesOf(kind) != nullptr) {
        return true;
    }
    // A rewind can retain the artwork while removing the enemy kind from its
    // active simulation roster. Replaying this birth must reactivate both,
    // exactly as the first uncached load did.
    if (kind >= 0 && !enemies.loadKind(kind)) {
        return false;
    }
    for (auto& cached : m_bodyCache) {
        if (cached->kind == kind) {
            cached->textures.restoreClock({});
            m_sim.bodies.push_back(cached.get());
            return true;
        }
    }
    ItemArchive* archive = kind <= kTempleBrood ? realmItems : enemies.archive(kind);
    if (archive == nullptr) {
        return false;
    }
    auto bodies = std::make_unique<Bodies>();
    bodies->kind = kind;
    bodies->archive = archive;
    bodies->textures.bind(archive->trees.textureAnimations(), archive->textures, device, lenders);
    const EnemyKind& info = enemyKind(kind);
    // The state's object: "GEN_GRU3", with the authored suffixes the original retries.
    // Whole is three; nought is the ruin.
    for (s32 state = 0; state <= kStates; ++state) {
        const std::string base =
            std::format("GEN_{}{}", kind <= kTempleBrood ? "SPECIAL" : info.prefix, state);
        // SPECIAL generators are multi-node authored trees in the realm archive,
        // not a model from one of the four enemy species they produce.
        if (const auto tree = archive->trees.find(base)) {
            bodies->trees[static_cast<usize>(state)] = archive->trees.tree(*tree);
            bodies->models[static_cast<usize>(state)].bind(bodies->trees[static_cast<usize>(state)],
                                                           archive->models, archive->textures,
                                                           device, lenders);
            bodies->models[static_cast<usize>(state)].setFrame(0, 0);
            continue;
        }
        if (const auto object = AssetScope(*archive).findObject(base)) {
            TreeInfo& tree = bodies->trees[static_cast<usize>(state)];
            tree.name = base;
            TreeNodeInfo node;
            node.name = base;
            node.object = object->name;
            // SetItemGeo gives every item's node the sort flag: an alpha-textured body such
            // as the vermin's skull pile is then blended by its texture's alpha after the
            // solids, rather than cut by coverage, which turns its faint halo opaque.
            node.objectFlags = object_flags::kSorted;
            tree.nodes.push_back(node);
            bodies->models[static_cast<usize>(state)].bind(tree, archive->models, archive->textures,
                                                           device, lenders);
        }
    }
    m_sim.bodies.push_back(bodies.get());
    m_bodyCache.push_back(std::move(bodies));
    return true;
}

bool Generators::bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
                      const WorldCollision* collision, const GeneratorScales& scales, s32 players,
                      std::span<const LevelEnemy> roster, s32 realm, ItemArchive* realmItems,
                      std::span<TextureSet* const> lenders) {
    clear();
    m_sim.collision = collision;
    const auto authored = itemSupportWorld(layout, collision);
    m_sim.scales = scales;
    m_sim.players = players;
    m_realmItems = realmItems;
    m_lenders.assign(lenders.begin(), lenders.end());
    m_records.clear();
    for (const ItemInfo& record : layout.itemInfos()) {
        if (record.type == ItemInfo::kGenerator) {
            m_records.push_back(record);
        }
    }
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    for (const ItemInstance& instance : layout.itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kGenerator) {
            continue;
        }
        const bool special = info.name == kSpecialRecord && (realm == 5 || realm == 6);
        const auto named = special ? std::optional<s32>{realm == 5 ? kTempleBrood : kHellBrood}
                                   : enemyKindOf(info.name);
        if (!named.has_value()) {
            continue;
        }
        // Small-class placements use the LOW body before the level remaps their species.
        const ItemInfo* body = &info;
        if (*named == kRatKind) {
            const auto low =
                std::ranges::find_if(layout.itemInfos(), [&](const ItemInfo& candidate) {
                    return candidate.type == info.type && candidate.name == "LOW" &&
                           (info.subtype <= 0 || candidate.subtype == info.subtype);
                });
            if (low != layout.itemInfos().end()) {
                body = &*low;
            }
        }
        const s32 specialTier = realm == 5 ? kTempleBroodStrength : kHellBroodStrength;
        const s32 authoredStrength = std::max(paramOf(instance, 0), 1);
        const s32 strength = special ? specialTier : authoredStrength;
        const s32 kind = levelKindOf(roster, *named, kGeneratorKindStrength);
        if (special) {
            for (const s32 species : realm == 5 ? kTempleKinds : kHellKinds) {
                enemies.loadKind(species);
            }
        }
        Placement placement;
        placement.info = &info;
        placement.body = body;
        placement.kind = kind;
        placement.strength = strength;
        placement.authoredStrength = authoredStrength;
        placement.algorithm = paramOf(instance, 1);
        placement.most = paramOf(instance, 2);
        placement.interval = paramOf(instance, 3);
        placement.minPlayers = instance.minPlayers;
        placement.alwaysActive = (info.activeType & ItemInfo::kAlwaysActive) != 0 ||
                                 (instance.flags & ItemInstance::kAlwaysActive) != 0;
        placement.position = instance.position;
        placement.rotation = instance.rotation;
        if (!stand(device, enemies, placement, authored ? &*authored : nullptr, realmItems,
                   lenders) &&
            m_sim.nextInstance == 0) {
            return false;
        }
    }
    syncFloors();
    m_roster.reserve(m_sim.generators.size());
    for (const auto& generator : m_sim.generators) {
        m_roster.push_back(generator);
    }
    m_sim.bound = true;
    return true;
}

bool Generators::stand(RenderDevice& device, Enemies& enemies, const Placement& placement,
                       const WorldCollision* authored, ItemArchive* realmItems,
                       std::span<TextureSet* const> lenders) {
    const ItemInfo& info = *placement.info;
    const ItemInfo& body = *placement.body;
    const s32 kind = placement.kind;
    if (!loadBodies(device, enemies, kind, realmItems, lenders) || m_sim.nextInstance == 0) {
        return false;
    }
    const GeneratorScales& scales = m_sim.scales;
    const WorldCollision* collision = m_sim.collision;
    Generator generator;
    generator.minPlayers = placement.minPlayers;
    generator.presence = shownToParty(placement.minPlayers, m_sim.players)
                             ? Generator::Presence::Shown
                             : Generator::Presence::Hidden;
    generator.kind = kind;
    // The strength is also the state it stands in: a strength-one generator is one state
    // from gone and looks it.
    generator.tier = placement.strength;
    generator.state = std::min(placement.strength, kStates);
    generator.algorithm = placement.algorithm;
    if (generator.algorithm < 0) {
        generator.algorithm = enemyKind(kind).algorithm;
    }
    const auto tierIndex =
        static_cast<usize>(std::clamp(placement.authoredStrength, 1, kStates) - 1);
    s32 most = placement.most;
    s32 interval = placement.interval;
    if (most == 0) {
        most = kDefaultMost[tierIndex];
    }
    if (interval == 0) {
        interval = kDefaultInterval[tierIndex];
    }
    // Scaled the way the original truncates them: to a whole count and interval.
    generator.most = static_cast<s32>(static_cast<f32>(most) * scales.most);
    generator.interval = static_cast<s32>(static_cast<f32>(interval) * scales.rate);
    generator.threshold = std::trunc(static_cast<f32>(body.hitPoints) * scales.health);
    // Durability and defaults use the authored strength before a special brood
    // replaces its troop tier. Damage and initial health are whole hit points.
    generator.health =
        std::trunc(static_cast<f32>(info.hitPoints * placement.authoredStrength) * scales.health);
    generator.armor = static_cast<f32>(info.armor);
    const Mat4 placed = itemPlacement(placement.position, placement.rotation);
    generator.position = placement.position;
    if (collision != nullptr && (info.collisionFlags & ItemInfo::kKeepsHeight) == 0) {
        if (const auto floor = collision->floorAt(placement.position, 3.0f, 6.0f)) {
            // AddItemSub uses the same floor lift as other placed items.
            // Ground portals otherwise share the floor's depth exactly.
            generator.position.y = floor->y + ItemFigure::kFloorLift;
        }
    }
    generator.yaw = portable::atan2(placed[2][0], placed[2][2]);
    generator.placement = placed;
    generator.placement[3] = Vec4{generator.position, 1};
    // do_items passes worldmat[2] to generate_enemy unchanged. Its horizontal
    // magnitude matters: a pitched generator has shorter exits, and a vertical
    // one births at its centre instead of inventing a horizontal forward axis.
    generator.direction = Vec3{placed[2][0], 0.0f, placed[2][2]};
    // SetItem copies the original offset before replacing a rat's info by LOW.
    generator.collisionOffset = info.collisionOffset;
    generator.targetRadius = body.radius;
    generator.targetHeight = body.height;
    // do_items (0x80061148) loads iteminfo+0x0c: radius, not height (+0x10).
    // generate_enemy adds the enemy's radius. START artwork is authored backwards
    // from that landing point, so a height-sized offset makes floor-portal births
    // visibly appear beside the portal rather than emerge through its centre.
    generator.clearance = body.radius;
    generator.patrolClearance = body.radius;
    generator.viewRadius = kGeneratorViewScale * std::max(info.radius, info.height);
    generator.alwaysActive = placement.alwaysActive;
    generator.box.centre = generator.position;
    generator.box.yaw = generator.yaw;
    generator.box.halfAcross = body.xSize > 0.0f ? body.xSize : body.radius;
    generator.box.halfAlong = body.zSize > 0.0f ? body.zSize : body.radius;
    generator.box.height = body.height;
    generator.box.cylinderRadius = body.collisionType == 1 ? body.radius : 0.0f;
    if (body.collisionType == 1 || body.collisionType == 3) {
        generator.box.enemyItem = Obstacle::ItemQuery{body.collisionType == 1, body.radius};
    }
    generator.box.solid = generator.presence == Generator::Presence::Shown;
    bindSupport(generator, info, placement.position,
                itemSupportAt(placement.position, collision, authored));
    generator.countdown = 0;
    generator.instance = m_sim.nextInstance;
    m_sim.generators.push_back(std::move(generator));
    ++m_sim.nextInstance;
    return true;
}

std::optional<s32> Generators::spawn(RenderDevice& device, Enemies& enemies, s32 kind, s32 tier,
                                     const Vec3& position, f32 yaw, ItemArchive* realmItems,
                                     const ItemInfo* record) {
    if (!m_sim.bound || !generatorKind(kind)) {
        return std::nullopt;
    }
    const bool brood = kind <= kTempleBrood;
    if (brood) {
        for (const s32 species : kind == kTempleBrood ? kTempleKinds : kHellKinds) {
            if (!enemies.loadKind(species)) {
                return std::nullopt;
            }
        }
    }
    // The record given, else the level's for the kind, any of its generator records failing
    // that, else stock measures: the figure is the kind's own either way.
    const std::string_view name = brood ? kSpecialRecord : enemyKind(kind).name;
    const auto named = std::ranges::find(m_records, name, &ItemInfo::name);
    ItemInfo stock;
    stock.type = ItemInfo::kGenerator;
    stock.name = std::string(name);
    stock.radius = kStockGeneratorRadius;
    stock.height = kStockGeneratorHeight;
    stock.hitPoints = kStockGeneratorHitPoints;
    const ItemInfo& info = record != nullptr          ? *record
                           : named != m_records.end() ? *named
                           : !m_records.empty()       ? m_records.front()
                                                      : stock;
    Placement placement;
    placement.info = &info;
    placement.body = &info;
    placement.kind = kind;
    placement.authoredStrength = std::clamp(tier, 1, kStates);
    placement.strength = brood ? (kind == kTempleBrood ? kTempleBroodStrength : kHellBroodStrength)
                               : placement.authoredStrength;
    placement.position = position;
    placement.rotation = Vec3{0.0f, yaw, 0.0f};
    if (!stand(device, enemies, placement, nullptr,
               realmItems != nullptr ? realmItems : m_realmItems, m_lenders)) {
        return std::nullopt;
    }
    syncFloors();
    return static_cast<s32>(m_sim.generators.size()) - 1;
}

std::optional<s32> Generators::spawnImported(RenderDevice& device, Enemies& enemies,
                                             const ImportedGenerator& imported,
                                             f32 difficultyGain) {
    const auto finite = [](const Vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    const auto& body = imported.body;
    if (!m_sim.bound || imported.kind < 0 || imported.kind >= kSwarmKindCount ||
        imported.tier < 1 || imported.tier > kStates || imported.minimumPlayers < 0 ||
        imported.minimumPlayers > 4 || !finite(imported.position) || !std::isfinite(imported.yaw) ||
        body.type != ItemInfo::kGenerator || !std::isfinite(body.radius) || body.radius <= 0 ||
        body.radius > 32 || !std::isfinite(body.height) || body.height <= 0 || body.height > 64 ||
        !std::isfinite(body.xSize) || body.xSize < 0 || body.xSize > 32 ||
        !std::isfinite(body.zSize) || body.zSize < 0 || body.zSize > 32 ||
        !finite(body.collisionOffset) || body.hitPoints <= 0 || body.hitPoints > 100000 ||
        body.armor < 0 || imported.algorithm < -1 || imported.algorithm > 32 ||
        imported.maximumBrood < 0 || imported.maximumBrood > 127 || imported.spawnInterval < 0 ||
        imported.spawnInterval > 127 || (body.collisionType != 1 && body.collisionType != 3)) {
        return std::nullopt;
    }
    const bool authoredHealth =
        std::ranges::any_of(imported.stageHealth, [](s32 health) { return health != 0; });
    ImportedHealth health;
    if (imported.breeding) {
        const auto& policy = *imported.breeding;
        const auto range = [](const Vec2& bounds) {
            return std::isfinite(bounds.x) && std::isfinite(bounds.y) && bounds.x > 0 &&
                   bounds.y >= bounds.x && bounds.y <= 100;
        };
        const f32 difficulty = policy.difficulty * difficultyGain;
        if (!std::isfinite(difficulty) || difficulty <= 0 || !range(policy.broodDifficulty) ||
            !range(policy.waitDifficulty) || !std::isfinite(policy.broodBonus) ||
            policy.broodBonus < 0 || policy.broodBonus > 100 || policy.maximumBrood < 1 ||
            policy.maximumBrood > 127 ||
            std::ranges::any_of(policy.broodByState,
                                [](s32 count) { return count < 1 || count > 127; }) ||
            policy.waitTicks[0] < 1 || policy.waitTicks[1] < policy.waitTicks[0] ||
            policy.waitTicks[1] > 36000 || !std::isfinite(policy.activeDistance) ||
            policy.activeDistance <= 0 || policy.activeDistance > kActiveDistance)
            return std::nullopt;
        ImportedHealth::Breeding resolved;
        const f32 population =
            std::clamp(difficulty, policy.broodDifficulty.x, policy.broodDifficulty.y) +
            policy.broodBonus;
        for (usize state = 0; state < resolved.broodByState.size(); ++state)
            resolved.broodByState[state] = std::clamp(
                static_cast<s32>(static_cast<f32>(policy.broodByState[state]) * population + .5f),
                1, policy.maximumBrood);
        const f32 pace = std::clamp(difficulty, policy.waitDifficulty.x, policy.waitDifficulty.y);
        for (usize end = 0; end < resolved.waitTicks.size(); ++end)
            resolved.waitTicks[end] =
                std::max(1, static_cast<s32>(static_cast<f32>(policy.waitTicks[end]) / pace));
        resolved.activeDistance = policy.activeDistance;
        health.breeding = resolved;
    }
    const bool mixed = imported.broodTier != 0;
    if ((mixed &&
         (imported.broodTier < 1 || imported.broodTier > 3 || imported.broodKinds.size() < 3 ||
          imported.broodKinds.size() > 4 ||
          std::ranges::any_of(imported.broodKinds,
                              [](s32 kind) { return kind < 0 || kind >= kSwarmKindCount; }) ||
          (!imported.broodAlgorithms.empty() &&
           (imported.broodAlgorithms.size() != imported.broodKinds.size() ||
            std::ranges::any_of(imported.broodAlgorithms,
                                [](s32 way) { return way < 0 || way > 32; }))))) ||
        (!mixed && (!imported.broodKinds.empty() || !imported.broodAlgorithms.empty()))) {
        return std::nullopt;
    }
    if (mixed) {
        for (const auto kind : imported.broodKinds) {
            if (!enemies.loadKind(kind)) {
                return std::nullopt;
            }
        }
        health.broodKinds = imported.broodKinds;
        health.broodAlgorithms = imported.broodAlgorithms;
        health.broodTier = imported.broodTier;
    }
    if (authoredHealth) {
        s32 total = 0;
        for (s32 state = 0; state < kStates; ++state) {
            const s32 amount = imported.stageHealth[static_cast<usize>(state)];
            if (amount < 0 || amount > 100000 ||
                (state < imported.tier ? amount == 0 : amount != 0)) {
                return std::nullopt;
            }
            total += amount;
            health.remaining[static_cast<usize>(state)] = static_cast<f32>(total);
        }
    }
    Placement placement;
    placement.info = &body;
    placement.body = &body;
    placement.kind = imported.kind;
    placement.strength = imported.tier;
    placement.authoredStrength = imported.tier;
    placement.position = imported.position;
    placement.rotation = Vec3{0, imported.yaw, 0};
    placement.minPlayers = imported.minimumPlayers;
    placement.algorithm = imported.algorithm;
    placement.most = imported.maximumBrood;
    placement.interval = imported.spawnInterval;
    placement.alwaysActive = imported.alwaysActive;
    if (!stand(device, enemies, placement, nullptr, m_realmItems, m_lenders)) {
        return std::nullopt;
    }
    syncFloors();
    const s32 id = static_cast<s32>(m_sim.generators.size()) - 1;
    m_externalBodies.push_back(instanceOf(id));
    if (authoredHealth || mixed || health.breeding) {
        health.instance = instanceOf(id);
        auto& generator = m_sim.generators.back();
        if (authoredHealth) {
            generator.health = health.remaining.back();
            generator.threshold = health.remaining.front();
        } else {
            for (usize state = 0; state < health.remaining.size(); ++state) {
                health.remaining[state] = generator.threshold * static_cast<f32>(state + 1);
            }
        }
        if (health.breeding)
            generator.most = health.breeding->broodByState[static_cast<usize>(generator.state - 1)];
        m_importedHealth.push_back(health);
        if (mixed && !m_sim.importedRandom) {
            m_sim.importedRandom.emplace(0x4c454745U);
        }
    }
    ++m_localEpoch; // earlier snapshots cannot predate an imported rule binding
    return id;
}

bool Generators::externallyPresented(s32 id) const {
    const u64 instance = instanceOf(id);
    return instance != 0 && std::ranges::find(m_externalBodies, instance) != m_externalBodies.end();
}

void Generators::bindSupport(Generator& generator, const ItemInfo& info, const Vec3& authored,
                             const WorldCollision* collision) {
    if (collision == nullptr || (info.collisionFlags & ItemInfo::kKeepsHeight) != 0) {
        return;
    }
    const auto floor = collision->floorAt(authored, 4, 10, 1);
    if (!floor) {
        return;
    }
    const auto transform = collision->objectTransform(floor->object);
    if (!transform) {
        return;
    }
    generator.boxOffset = Vec3{glm::inverse(generator.placement) * Vec4{generator.box.centre, 1}};
    generator.boxYaw = generator.box.yaw - generator.yaw;
    Mat4 placement = generator.placement;
    placement[3] = Vec4{authored.x, floor->y + ItemFigure::kFloorLift, authored.z, 1};
    generator.support = floor->object;
    generator.supportLocal = glm::inverse(*transform) * placement;
}

void Generators::syncFloors() {
    if (m_sim.collision == nullptr) {
        return;
    }
    for (Generator& generator : m_sim.generators) {
        if (generator.support < 0) {
            continue;
        }
        const auto transform = m_sim.collision->objectTransform(generator.support);
        if (!transform) {
            generator.support = -1;
            continue;
        }
        // Disabling a floor's collision does not detach its scenery children.
        generator.placement = *transform * generator.supportLocal;
        generator.position = Vec3{generator.placement[3]};
        generator.yaw = portable::atan2(generator.placement[2].x, generator.placement[2].z);
        generator.direction = Vec3{generator.placement[2].x, 0, generator.placement[2].z};
        generator.box.centre = Vec3{generator.placement * Vec4{generator.boxOffset, 1}};
        generator.box.yaw = generator.yaw + generator.boxYaw;
        if (generator.bossFigure.has_value()) {
            generator.bossFigure->placeAt(generator.placement);
        }
    }
}

bool Generators::placeBoss(RenderDevice& device, const ItemInfo& info, ItemArchive& items,
                           Enemies& enemies, s32 kind, const Mat4& placement,
                           const WorldCollision* collision, std::string_view tree, bool settled) {
    if (m_sim.nextInstance == 0 || info.type != ItemInfo::kGenerator || info.name != "BOSSGEN" ||
        kind < 0 || kind >= kEnemyKindCount || !enemies.loadKind(kind)) {
        return false;
    }
    Generator generator;
    generator.boss = true;
    generator.kind = kind;
    generator.tier = 1;
    generator.state = 1;
    // The default instance has strength one and AI zero. Its forty-tick item
    // animation startup is separate from the brood counter, which starts ready.
    generator.algorithm = 0;
    generator.countdown = 0;
    generator.viewRadius = kGeneratorViewScale * std::max(info.radius, info.height);
    generator.alwaysActive = (info.activeType & ItemInfo::kAlwaysActive) != 0;
    generator.most = static_cast<s32>(static_cast<f32>(kDefaultMost[0]) * m_sim.scales.most);
    generator.interval =
        static_cast<s32>(static_cast<f32>(kDefaultInterval[0]) * m_sim.scales.rate);
    generator.health = generator.threshold =
        std::trunc(static_cast<f32>(info.hitPoints) * m_sim.scales.health);
    generator.armor = static_cast<f32>(info.armor);
    ItemInstance instance;
    instance.position = Vec3{placement[3]};
    const f32 yaw = portable::atan2(placement[2].x, placement[2].z);
    instance.rotation.y = -yaw;
    // AtreeMatchAnyHeader also searches the summoned species' loaded archive.
    // The Genie's BOSSGEN tree lives with WIND, not with DJINN or the level items.
    AssetScope scope(items);
    if (ItemArchive* brood = enemies.archive(kind); brood != nullptr) {
        scope.add(*brood);
    }
    ItemArchive* art = scope.archiveForTree(tree);
    // Keep an unstarted native value with a shared artwork cache. Replaying a
    // birth starts from the same clock/generation without resolving art again.
    ItemArchive* archive = art != nullptr ? art : &items;
    auto prototype = std::ranges::find_if(m_bossPrototypes, [&](const BossPrototype& value) {
        return value.archive == archive && value.device == &device && value.tree == tree;
    });
    if (prototype == m_bossPrototypes.end()) {
        m_bossPrototypes.push_back({archive, &device, std::string{tree}, {}});
        prototype = std::prev(m_bossPrototypes.end());
    }
    generator.bossFigure = prototype->figure;
    // A missing BOSSGEN tree does not prevent PlaceItem from creating its gameplay object.
    generator.bossFigure->place(device, *archive, tree, instance, collision);
    generator.bossFigure->play(0,
                               !settled ? ItemFigure::Playback::Loop : ItemFigure::Playback::Once);
    if (settled) {
        // The projectile has already finished its landing before it leaves the generator.
        generator.bossFigure->update(static_cast<f32>(generator.bossFigure->ticksOf(0)) / 60.0f);
    }
    generator.position = generator.bossFigure->position();
    generator.placement = placement;
    generator.placement[3] = Vec4{generator.position, 1};
    generator.yaw = yaw;
    generator.direction = Vec3{generator.placement[2].x, 0, generator.placement[2].z};
    generator.collisionOffset = info.collisionOffset;
    generator.targetRadius = info.radius;
    generator.targetHeight = info.height;
    generator.clearance = info.radius;
    generator.patrolClearance = info.radius;
    generator.box = generator.bossFigure->obstacle(info);
    bindSupport(generator, info, instance.position, collision);
    if (collision != nullptr) {
        m_sim.collision = collision;
    }
    generator.instance = m_sim.nextInstance;
    m_sim.generators.push_back(std::move(generator));
    ++m_sim.nextInstance;
    syncFloors();
    return true;
}

void Generators::clear() {
    ++m_localEpoch;
    m_sim.bound = false;
    m_roster.clear();
    m_externalBodies.clear();
    m_importedHealth.clear();
    m_records.clear();
    m_realmItems = nullptr;
    m_lenders.clear();
    m_sim.collision = nullptr;
    m_sim.view.reset();
    m_sim.attentionGate = false;
    m_sim.specialBirth = 0;
    m_sim.importedRandom.reset();
    m_sim.nextInstance = 1;
    m_sim.generators.clear();
    for (auto& bodies : m_sim.bodies) {
        for (TreeModel& model : bodies->models) {
            model.clear();
        }
    }
    m_sim.bodies.clear();
    m_bodyCache.clear();
    m_bossPrototypes.clear();
}

bool Generators::seen(const Generator& generator) const {
    if (!m_sim.view) {
        return true;
    }
    // SetItem's collision and attention anchors share the lifted, rotated offset.
    const Vec3 point =
        TargetAssist::itemAcquisition(generator.placement, generator.collisionOffset,
                                      generator.targetRadius, generator.targetHeight, 1)
            .point;
    // do_items applies the attention gate in normal/debug camera modes (1/2), not the
    // legacy camera used by bosses: an expanded sphere alone admits distant huts. Under
    // solo cameras any player's camera counts, each with its own point.
    return m_sim.view->sees(point, generator.viewRadius,
                            m_sim.attentionGate ? kAttentionRange : 0.0f);
}

void Generators::updatePresence(Generator& generator, bool seen) const {
    // do_items' minoff gate changes an existing item's presence offscreen. A hidden
    // placement encountered on screen is suppressed for the rest of the level.
    if (generator.presence == Generator::Presence::Hidden) {
        if (seen) {
            generator.presence = Generator::Presence::Suppressed;
        } else if (shownToParty(generator.minPlayers, m_sim.players)) {
            generator.presence = Generator::Presence::Shown;
        }
    } else if (generator.presence == Generator::Presence::Shown && !seen &&
               !shownToParty(generator.minPlayers, m_sim.players)) {
        generator.presence = Generator::Presence::Hidden;
    }
    generator.box.solid = generator.presence == Generator::Presence::Shown && generator.state > 0;
}

void Generators::applyBroodEvents(Enemies& enemies) {
    for (const auto& event : enemies.takeGeneratorEvents()) {
        if (event.generator < 0 || static_cast<usize>(event.generator) >= m_sim.generators.size()) {
            continue;
        }
        auto& generator = m_sim.generators[static_cast<usize>(event.generator)];
        switch (event.kind) {
        case EnemyGeneratorEvent::Kind::Born: ++generator.living; break;
        case EnemyGeneratorEvent::Kind::Detached:
            generator.living = std::max(generator.living - 1, 0);
            break;
        case EnemyGeneratorEvent::Kind::PatrolHit: generator.algorithm = 0; break;
        case EnemyGeneratorEvent::Kind::PatrolDetached:
            generator.algorithm = 0;
            generator.living = 0;
            break;
        }
    }
}

void Generators::update(s32 ticks, Enemies& enemies, std::span<const EnemyView> players,
                        std::span<const Obstacle> obstacles, bool timeStopped) {
    syncFloors();
    if (ticks <= 0) {
        return;
    }
    const SwarmTuning tuning = m_swarm != nullptr ? m_swarm->tuning() : SwarmTuning{};
    // How near the party stands to a spot, for the frenzy's reach.
    const auto nearestPlayer = [players](const Vec3& position) {
        f32 nearest = std::numeric_limits<f32>::max();
        for (const EnemyView& view : players) {
            if (!view.hidden) {
                nearest = std::min(nearest, glm::distance(view.position, position));
            }
        }
        return nearest;
    };
    // Texture cycles belong to the archive, not to each placed generator or its
    // current damage state. Keep them running even when its brood cannot spawn.
    for (auto& bodies : m_sim.bodies) {
        bodies->textures.advance(static_cast<f32>(ticks) / 60.0f);
    }
    // Refresh the entire obstacle roster before any generator attempts a birth.
    for (Generator& generator : m_sim.generators) {
        updatePresence(generator, seen(generator));
    }
    // uncouple_enemy releases quota before the death animation. A patrol's
    // departure resets the counter, which cannot be reconstructed by a census.
    applyBroodEvents(enemies);
    for (usize g = 0; g < m_sim.generators.size(); ++g) {
        Generator& generator = m_sim.generators[g];
        const bool visible = seen(generator);
        if (generator.presence != Generator::Presence::Shown) {
            continue;
        }
        if (generator.bossFigure.has_value() && generator.state > 0) {
            generator.bossFigure->update(static_cast<f32>(ticks) / 60.0f);
        }
        if (generator.state <= 0 || generator.tier <= 0) {
            continue;
        }
        const bool patrol = generator.algorithm == kPatrolWay;
        const auto imported =
            std::ranges::find(m_importedHealth, generator.instance, &ImportedHealth::instance);
        const auto* breeding = imported != m_importedHealth.end() && imported->breeding
                                   ? &*imported->breeding
                                   : nullptr;
        if (!patrol && !visible && !generator.alwaysActive) {
            continue;
        }
        // do_items checks the living brood quota before generate_now ticks its timer.
        // A full brood freezes the remaining wait; a death frees a slot, not a free birth.
        // Patrol generators instead keep one sentry, with no ordinary quota or wait.
        // The mechanics may let a brood run past its quota, and a generator near the party
        // breed past any quota at all.
        const auto quota = static_cast<s32>(static_cast<f32>(generator.most) * tuning.broodScale);
        const bool frenzied =
            tuning.frenzyReach > 0.0f && nearestPlayer(generator.position) <= tuning.frenzyReach;
        if (patrol ? generator.living > 0
                   : generator.most <= 0 || (!frenzied && generator.living >= quota)) {
            continue;
        }
        if (breeding &&
            (timeStopped || nearestPlayer(generator.position) > breeding->activeDistance)) {
            continue;
        }
        if (!patrol && generator.countdown > 0) {
            generator.countdown -= ticks;
            // Range-based native clocks admit a birth on the tick reaching
            // zero; the ordinary GDL clock retains its following-tick gate.
            if (!breeding || generator.countdown > 0)
                continue;
        }
        if (timeStopped) {
            continue;
        }
        // Ordinary broods require a nearby player; a lone patrol sentry starts offscreen
        // as well, without waiting for the party to approach its post.
        bool near = false;
        for (const EnemyView& view : players) {
            near = near || (!view.hidden &&
                            glm::distance(view.position, generator.position) <= kActiveDistance);
        }
        if (!patrol && !near) {
            continue;
        }
        EnemySpawn spawn;
        spawn.kind = generator.kind;
        spawn.tier = generator.tier;
        spawn.algorithm = generator.algorithm;
        if (generator.kind == kTempleBrood || generator.kind == kHellBrood) {
            const usize at = m_sim.specialBirth++ % kTempleKinds.size();
            spawn.kind = generator.kind == kTempleBrood ? kTempleKinds[at] : kHellKinds[at];
            spawn.tier = generator.kind == kTempleBrood ? 2 : 3;
            spawn.algorithm = generator.kind == kTempleBrood ? 7 : kHellAlgorithms[at];
            spawn.allBirthDirections = true;
        }
        if (imported != m_importedHealth.end() && imported->broodTier != 0) {
            // Imported native mixed broods select an authored species.
            // Their RNG belongs to this simulation snapshot and never advances
            // the ordinary GDL brood cycle or any global/presentation clock.
            const u32 roll = (*m_sim.importedRandom)() >> 17U;
            const usize choice = (roll * static_cast<u32>(imported->broodKinds.size())) >> 15U;
            spawn.kind = imported->broodKinds[choice];
            spawn.tier = imported->broodTier;
            spawn.algorithm = imported->broodAlgorithms.empty() ? enemyKind(spawn.kind).algorithm
                                                                : imported->broodAlgorithms[choice];
            spawn.allBirthDirections = true;
        }
        spawn.position = generator.position;
        spawn.direction = generator.direction;
        spawn.clearance = patrol ? generator.patrolClearance : generator.clearance;
        spawn.patrolBirth = patrol;
        // A frenzied hut's births take any slot, replacing even a body in view.
        spawn.priority = frenzied            ? EnemySpawn::Priority::Visible
                         : visible || patrol ? EnemySpawn::Priority::Offscreen
                                             : EnemySpawn::Priority::FreeSlotOnly;
        spawn.generator = static_cast<s32>(g);
        spawn.frenzied = frenzied;
        if (generator.algorithm == kZigZagWay) {
            spawn.zigZagSide = (generator.bred & 1) == 0 ? 1 : -1;
        }
        std::vector<Obstacle> birthObstacles{obstacles.begin(), obstacles.end()};
        for (usize other = 0; other < m_sim.generators.size(); ++other) {
            if (other != g && standing(static_cast<s32>(other))) {
                birthObstacles.push_back(m_sim.generators[other].box);
            }
        }
        const auto child = enemies.spawn(spawn, players, birthObstacles);
        // Recycled slots release their previous generator even when birth fails.
        applyBroodEvents(enemies);
        if (!child.has_value()) {
            continue;
        }
        ++generator.bred;
        if (patrol) {
            continue;
        }
        // The next takes longer, the countdown stretched by a share that grows a birth at a
        // time and wraps, and cut by the mechanics' breeding pace.
        generator.countdown =
            breeding
                ? static_cast<s32>((static_cast<f32>(breeding->waitTicks[0]) +
                                    generator.ratio * static_cast<f32>(breeding->waitTicks[1] -
                                                                       breeding->waitTicks[0])) /
                                   tuning.breedingPace)
                : static_cast<s32>(kCountdownScale * static_cast<f32>(generator.interval) *
                                   (1.0f + generator.ratio) / tuning.breedingPace);
        generator.ratio += 1.0f / (2.0f * static_cast<f32>(generator.most));
        if (generator.ratio > 1.0f) {
            generator.ratio = 0.0f;
        }
    }
}

s32 Generators::stateFor(const Generator& generator, bool destroyed) const {
    if (destroyed || generator.health <= 0.0f) {
        return 0;
    }
    const auto imported =
        std::ranges::find(m_importedHealth, generator.instance, &ImportedHealth::instance);
    if (imported != m_importedHealth.end()) {
        for (s32 state = 0; state < kStates; ++state) {
            if (generator.health <= imported->remaining[static_cast<usize>(state)]) {
                return state + 1;
            }
        }
        return kStates;
    }
    if (generator.health <= generator.threshold) {
        return 1;
    }
    if (generator.health <= generator.threshold * 2.0f) {
        return 2;
    }
    return 3;
}

std::optional<GeneratorEvent> Generators::strike(s32 id, f32 power, [[maybe_unused]] s32 byPlayer) {
    if (id < 0 || static_cast<usize>(id) >= m_sim.generators.size()) {
        return std::nullopt;
    }
    Generator& generator = m_sim.generators[static_cast<usize>(id)];
    if (generator.state <= 0 || generator.presence != Generator::Presence::Shown ||
        generator.armor < 0.0f) {
        return std::nullopt;
    }
    f32 amount = power - generator.armor;
    if (amount <= 0.0f) {
        amount = 1.0f;
    }
    generator.health = std::max(generator.health - std::round(amount), 0.0f);
    const s32 state = stateFor(generator, false);
    const bool changed = state != generator.state;
    generator.state = state;
    // A crumbling state is also the strength it breeds at from then on, and it breeds twice
    // as many; a caster brood goes back to seeking (fn_8005C1DC).
    if (state != generator.tier) {
        generator.tier = state;
        if (state > 0) {
            const auto imported =
                std::ranges::find(m_importedHealth, generator.instance, &ImportedHealth::instance);
            if (imported != m_importedHealth.end() && imported->breeding)
                generator.most = imported->breeding->broodByState[static_cast<usize>(state - 1)];
            else
                generator.most *= 2;
        }
        if (std::ranges::find(kCasterAlgorithms, generator.algorithm) != kCasterAlgorithms.end()) {
            generator.algorithm = 0;
        }
    }
    GeneratorEvent event;
    event.generator = id;
    event.kind = generator.kind;
    event.state = state;
    event.position = generator.position;
    event.placement = generator.placement;
    event.stateChanged = changed;
    event.destroyed = state == 0;
    event.instance = generator.instance;
    if (event.destroyed) {
        generator.box.solid = false;
    }
    return event;
}

std::optional<s32> Generators::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    for (usize g = 0; g < m_sim.generators.size(); ++g) {
        const Generator& generator = m_sim.generators[g];
        if (generator.state <= 0 || generator.presence != Generator::Presence::Shown) {
            continue;
        }
        const f32 reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        const Vec3 centre = generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f};
        const f32 t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + reach &&
            std::abs(nearest.y - centre.y) <= radius + 0.5f * generator.box.height + 1.0f) {
            const f32 distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = static_cast<s32>(g);
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<s32> Generators::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (usize g = 0; g < m_sim.generators.size(); ++g) {
        const Generator& generator = m_sim.generators[g];
        if (generator.state <= 0 || generator.presence != Generator::Presence::Shown) {
            continue;
        }
        const f32 reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        if (glm::length(generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f} -
                        centre) <= radius + reach) {
            out.push_back(static_cast<s32>(g));
        }
    }
    return out;
}

std::vector<Obstacle> Generators::obstacles() const {
    std::vector<Obstacle> out;
    for (const Generator& generator : m_sim.generators) {
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown) {
            out.push_back(generator.box);
        }
    }
    return out;
}

std::vector<Generators::Contact> Generators::identifiedObstacles() const {
    std::vector<Contact> out;
    for (usize index = 0; index < m_sim.generators.size(); ++index) {
        const auto& generator = m_sim.generators[index];
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown) {
            out.push_back({static_cast<u32>(index), generator.box});
        }
    }
    return out;
}

std::vector<Obstacle> Generators::enemyObstacles() const {
    std::vector<Obstacle> out;
    for (const Generator& generator : m_sim.generators) {
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown &&
            generator.kind != kWormKind) {
            out.push_back(generator.box);
        }
    }
    return out;
}

void Generators::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                      f32 presentationAlpha, TreeModel::Pass pass) {
    for (usize index = 0; index < m_sim.generators.size(); ++index) {
        const Generator& generator = m_sim.generators[index];
        if (generator.presence != Generator::Presence::Shown ||
            externallyPresented(static_cast<s32>(index))) {
            continue;
        }
        if (generator.boss) {
            if (generator.state > 0 && generator.bossFigure.has_value()) {
                generator.bossFigure->draw(device, clip, lighting, 1.0f, 1.0f, nullptr, pass);
            }
            continue;
        }
        Bodies* bodies = bodiesOf(generator.kind);
        if (bodies == nullptr) {
            continue;
        }
        const auto state = static_cast<usize>(std::clamp(generator.state, 0, kStates));
        TreeModel& model = bodies->models[state];
        if (!model.bound()) {
            continue;
        }
        bodies->textures.apply(model, bodies->trees[state], 0, 0.0f,
                               bodies->textures.presentationOffset(presentationAlpha));
        model.setHighlight(static_cast<s32>(index) == m_highlightedId ? m_highlight : nullptr);
        model.draw(device, clip, generator.placement, lighting, {}, nullptr, 1.0f, pass);
        model.setHighlight(nullptr); // the body is its kind's, shared
    }
}

std::vector<Generators::BodyResource> Generators::bodyResources() const {
    std::vector<BodyResource> result;
    for (const auto& bodies : m_sim.bodies) {
        for (s32 state = 0; state <= kStates; ++state) {
            const auto index = static_cast<usize>(state);
            if (bodies->models[index].bound()) {
                result.push_back({bodies->kind, state, &bodies->trees[index],
                                  &bodies->models[index], &bodies->textures});
            }
        }
    }
    std::ranges::sort(result, {},
                      [](const auto& entry) { return std::pair{entry.kind, entry.state}; });
    return result;
}

std::vector<Generators::Presentation> Generators::presentation() const {
    std::vector<Presentation> result;
    for (usize index = 0; index < m_sim.generators.size(); ++index) {
        if (!bodyShown(static_cast<s32>(index)) || externallyPresented(static_cast<s32>(index))) {
            continue;
        }
        const auto& generator = m_sim.generators[index];
        const auto* bodies = bodiesOf(generator.kind);
        result.push_back({index, generator.kind, generator.state, generator.placement,
                          bodies != nullptr ? static_cast<f32>(bodies->textures.frame()) : 0,
                          generator.bossFigure ? &*generator.bossFigure : nullptr});
    }
    return result;
}

bool Generators::standing(s32 id) const {
    return id >= 0 && static_cast<usize>(id) < m_sim.generators.size() &&
           m_sim.generators[static_cast<usize>(id)].state > 0 &&
           m_sim.generators[static_cast<usize>(id)].presence == Generator::Presence::Shown;
}

bool Generators::bodyShown(s32 id) const {
    if (id < 0 || static_cast<usize>(id) >= m_sim.generators.size()) {
        return false;
    }
    const Generator& generator = m_sim.generators[static_cast<usize>(id)];
    if (generator.presence != Generator::Presence::Shown) {
        return false;
    }
    if (generator.boss) {
        return generator.state > 0 && generator.bossFigure.has_value() &&
               generator.bossFigure->hasFigure();
    }
    const Bodies* bodies = bodiesOf(generator.kind);
    return bodies != nullptr &&
           bodies->models[static_cast<usize>(std::clamp(generator.state, 0, kStates))].bound();
}

s32 Generators::stateOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].state;
}
f32 Generators::healthOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].health;
}
s32 Generators::kindOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].kind;
}
s32 Generators::tierOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].tier;
}
s32 Generators::algorithmOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].algorithm;
}
s32 Generators::mostOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].most;
}
s32 Generators::intervalOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].interval;
}
s32 Generators::countdownOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].countdown;
}
s32 Generators::bredOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].bred;
}
const Vec3& Generators::positionOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].position;
}
MissileTarget Generators::target(s32 index, s32 id) const {
    const auto& generator = m_sim.generators.at(static_cast<usize>(index));
    MissileTarget result{id, generator.position,
                         std::max(generator.box.halfAcross, generator.box.halfAlong),
                         generator.box.height};
    result.acquisition =
        TargetAssist::itemAcquisition(generator.placement, generator.collisionOffset,
                                      generator.targetRadius, generator.targetHeight, 1);
    result.acquisition->enabled = generator.armor != -1;
    return result;
}

const Obstacle& Generators::boxOf(s32 id) const {
    return m_sim.generators[static_cast<usize>(id)].box;
}

} // namespace gdl::game
