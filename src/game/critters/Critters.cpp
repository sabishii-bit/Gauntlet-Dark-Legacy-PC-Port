#include "game/critters/Critters.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <tuple>
#include <type_traits>
#include <utility>

#include "game/critters/Gargoyle.h"
#include "game/critters/General.h"
#include "game/critters/Golem.h"
#include "game/level/NativeStateResources.h"
namespace gdl::game {
namespace {
void appendConfiguration(NativeStateDigest& digest, const WorldCollision* collision,
                         const HazardSurfaces* hazards, const EnemyScales& scales, char realm,
                         std::string_view gargoyleForm, bool open) {
    // These bindings govern a future birth even while all actor slots are empty.
    // Native snapshots require the same load epoch and collision/hazard owners.
    digest.resource(collision);
    digest.resource(hazards);
    digest.values(scales, realm, gargoyleForm, open);
}
} // namespace

void Critters::appendSimulationDigest(
    NativeStateDigest& digest, const Simulation& state,
    std::optional<std::span<const TextureAnimator::Clock>> savedClocks) {
    visitNativeFields<10>(state, [&]<class... Fields>(const Fields&... fields) {
        const auto append = [&]<class T>(const T& field) {
            if constexpr (std::is_same_v<T, std::vector<CombatantAssets*>>) {
                digest.wide(field.size());
                for (usize i = 0; i < field.size(); ++i) {
                    digest.resource(field[i]);
                    if (field[i] == nullptr) {
                        digest.invalidate();
                        return;
                    }
                    digest.fields<3>(savedClocks ? (*savedClocks)[i] : field[i]->textures.clock());
                }
            } else {
                digest.value(field);
            }
        };
        (append(fields), ...);
    });
}

bool Critters::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("Critters.native.1");
    appendConfiguration(digest, m_collision, m_hazards, m_scales, m_realm, m_gargoyleForm,
                        m_device != nullptr);
    appendSimulationDigest(digest, m_sim);
    for (const auto& fighter : m_critters) {
        if (!fighter.appendNativeDigest(digest)) {
            return false;
        }
    }
    return digest.good();
}

bool Critters::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("Critters.native.1");
    appendConfiguration(digest, saved.m_collision, saved.m_hazards, m_scales, m_realm,
                        m_gargoyleForm, m_device != nullptr);
    appendSimulationDigest(digest, saved.m_state,
                           std::span<const TextureAnimator::Clock>{saved.m_clocks});
    for (usize i = 0; i < m_critters.size(); ++i) {
        if (!m_critters[i].appendLocalDigest(digest, saved.m_actors[i])) {
            return false;
        }
    }
    return digest.good();
}

bool Critters::registerNativeResources(NativeStateResources& resources,
                                       std::string_view stablePrefix) const {
    if (!resources.registerLookouts(m_sim.lookouts, std::string(stablePrefix) + "/lookouts")) {
        return false;
    }
    for (const auto& stock : m_stockCache) {
        if (!resources.registerCombatantAssets(*stock,
                                               std::format("{}/{}/{}/{}", stablePrefix,
                                                           static_cast<s32>(stock->definition.kind),
                                                           stock->definition.name, stock->realm))) {
            return false;
        }
    }
    return resources.good();
}

Critters::~Critters() {
    close();
}
bool Critters::saveLocal(LocalSnapshot& snapshot) const {
    snapshot.m_owner.reset();
    snapshot.m_state = m_sim;
    snapshot.m_epoch = m_localEpoch;
    snapshot.m_collision = m_collision;
    snapshot.m_hazards = m_hazards;
    snapshot.m_clocks.resize(m_sim.stocks.size());
    snapshot.m_assets.resize(m_sim.stocks.size());
    for (usize i = 0; i < m_sim.stocks.size(); ++i) {
        snapshot.m_clocks[i] = m_sim.stocks[i]->textures.clock();
        snapshot.m_assets[i] = m_sim.stocks[i]->m_localBinding;
    }
    for (usize i = 0; i < m_critters.size(); ++i) {
        if (!m_critters[i].saveLocal(snapshot.m_actors[i])) {
            return false;
        }
    }
    snapshot.m_owner = m_localOwner;
    return true;
}
bool Critters::canRestoreLocal(const LocalSnapshot& snapshot) const {
    if (snapshot.m_owner.lock() != m_localOwner || snapshot.m_epoch != m_localEpoch ||
        snapshot.m_collision != m_collision || snapshot.m_hazards != m_hazards ||
        snapshot.m_clocks.size() != snapshot.m_state.stocks.size() ||
        snapshot.m_assets.size() != snapshot.m_state.stocks.size() ||
        std::ranges::any_of(snapshot.m_assets, [](const auto& asset) { return asset.expired(); })) {
        return false;
    }
    for (usize i = 0; i < m_critters.size(); ++i) {
        if (!m_critters[i].canRestoreLocal(snapshot.m_actors[i])) {
            return false;
        }
    }
    return true;
}
bool Critters::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    if (!canRestoreLocal(left) || !canRestoreLocal(right)) {
        return false;
    }
    bool equal = true;
    visitNativeFields<10>(left.m_state, [&](const auto&... first) {
        visitNativeFields<10>(right.m_state, [&](const auto&... second) {
            equal = std::tie(first...) == std::tie(second...);
        });
    });
    if (!equal || left.m_clocks != right.m_clocks) {
        return false;
    }
    // Inactive slots still carry incarnation counters and queued native state.
    for (usize i = 0; i < m_critters.size(); ++i) {
        if (!m_critters[i].equalLocal(left.m_actors[i], right.m_actors[i])) {
            return false;
        }
    }
    return true;
}
bool Critters::restoreLocal(const LocalSnapshot& snapshot) {
    if (!canRestoreLocal(snapshot)) {
        return false;
    }
    // Copy into the existing route object: patrols borrow its stable address.
    m_sim = snapshot.m_state;
    for (usize i = 0; i < m_sim.stocks.size(); ++i) {
        m_sim.stocks[i]->textures.restoreClock(snapshot.m_clocks[i]);
    }
    for (usize i = 0; i < m_critters.size(); ++i) {
        m_critters[i].restoreLocalValues(snapshot.m_actors[i]);
    }
    return true;
}
std::vector<CombatantAssets*> Critters::resources() {
    std::vector<CombatantAssets*> result;
    result.reserve(m_sim.stocks.size());
    for (auto& stock : m_sim.stocks) {
        result.push_back(stock);
    }
    return result;
}
std::vector<const CombatantAssets*> Critters::resources() const {
    std::vector<const CombatantAssets*> result;
    result.reserve(m_sim.stocks.size());
    for (const auto& stock : m_sim.stocks) {
        result.push_back(stock);
    }
    return result;
}
void Critters::open(RenderDevice& device, const std::filesystem::path& root,
                    const WorldCollision* collision, const EnemyScales& scales, char realm,
                    std::span<TextureSet* const> textureLenders, std::string_view gargoyleForm) {
    close();
    m_device = &device;
    m_root = root;
    m_textureLenders.assign(textureLenders.begin(), textureLenders.end());
    m_collision = collision;
    m_scales = scales;
    m_realm = static_cast<char>(std::toupper(static_cast<unsigned char>(realm)));
    m_gargoyleForm = gargoyleForm;
}
void Critters::close() {
    ++m_localEpoch;
    m_checkpointBinding = std::make_shared<const u8>(0);
    for (auto& actor : m_critters) {
        actor.clear();
    }
    m_sim.stocks.clear();
    m_stockCache.clear();
    m_textureLenders.clear();
    m_gargoyleForm.clear();
    m_sim.blows.clear();
    m_sim.grabs.clear();
    m_sim.losses.clear();
    m_sim.cues.clear();
    m_sim.spews.clear();
    m_sim.shots.clear();
    m_sim.rams.clear();
    m_sim.pushes.clear();
    m_hitFlash = nullptr;
    m_device = nullptr;
    m_collision = nullptr;
    m_hazards = nullptr;
}
CombatantAssets* Critters::stockFor(const CombatantDefinition& definition, char realm) {
    const char costume = realm != 0 ? realm : m_realm;
    for (auto& stock : m_sim.stocks) {
        if (stock->definition.name == definition.name &&
            stock->definition.kind == definition.kind && stock->realm == costume) {
            return stock;
        }
    }
    CombatantAssets* active = nullptr;
    for (const auto& stock : m_stockCache) {
        if (stock->definition.name == definition.name &&
            stock->definition.kind == definition.kind && stock->realm == costume) {
            active = stock.get();
            break;
        }
    }
    if (active == nullptr) {
        if (m_device == nullptr) {
            return nullptr;
        }
        auto stock = std::make_unique<CombatantAssets>();
        if (!stock->load(*m_device, m_root, definition, costume, m_textureLenders)) {
            return nullptr;
        }
        active = stock.get();
        m_stockCache.push_back(std::move(stock));
    }
    // A speculative first use may have already loaded this archive. Re-entry
    // begins at the same native clock without reloading/rebinding the artwork.
    active->textures.restoreClock({});
    m_sim.stocks.push_back(active);
    m_checkpointBinding = std::make_shared<const u8>(0);
    return active;
}
CombatantDefinition Critters::definitionOf(CombatantKind kind, std::string_view form) const {
    switch (kind) {
    case CombatantKind::Golem: return Golem::definition(m_realm);
    case CombatantKind::General: return General::definition();
    case CombatantKind::Gargoyle:
        return Gargoyle::definition(form.empty() ? std::string_view(m_gargoyleForm) : form);
    default: return CombatantDefinition{};
    }
}
std::optional<s32> Critters::spawnGolem(const Vec3& position, f32 yaw, char realm) {
    return spawn(Golem::definition(realm != 0 ? realm : m_realm), position, yaw, 0.0f, realm);
}
std::optional<s32> Critters::spawnGeneral(const Vec3& position, f32 yaw, f32 sight, char realm) {
    return spawn(General::definition(), position, yaw, sight, realm);
}
std::optional<s32> Critters::spawnGargoyle(const Vec3& position, f32 yaw, std::string_view form,
                                           char realm) {
    return spawn(definitionOf(CombatantKind::Gargoyle, form), position, yaw, 0.0f, realm);
}
std::optional<s32> Critters::spawn(CombatantKind kind, const Vec3& position, f32 yaw,
                                   std::string_view form, f32 sight) {
    const CombatantDefinition definition = definitionOf(kind, form);
    if (definition.kind == CombatantKind::Unknown) {
        return std::nullopt;
    }
    return spawn(definition, position, yaw, sight);
}
ItemArchive* Critters::archiveFor(CombatantKind kind, std::string_view form) {
    const CombatantDefinition definition = definitionOf(kind, form);
    if (definition.kind == CombatantKind::Unknown) {
        return nullptr;
    }
    CombatantAssets* stock = stockFor(definition);
    return stock != nullptr ? &stock->archive : nullptr;
}
std::optional<s32> Critters::spawn(const CombatantDefinition& definition, const Vec3& position,
                                   f32 yaw, f32 sight, char realm) {
    auto* stock = stockFor(definition, realm);
    if (stock == nullptr) {
        return std::nullopt;
    }
    for (s32 id = 0; id < kMost; ++id) {
        auto& actor = m_critters[static_cast<usize>(id)];
        if (!actor.present()) {
            if (actor.spawn(*stock, id, position, yaw, m_collision, m_scales,
                            realm != 0 ? realm : m_realm)) {
                actor.setHazards(m_hazards);
                // A general takes up its round of the lookouts where it is placed, seeing
                // only as far as its placement allows meanwhile (visrad at the level's scale).
                if (definition.patrols && !m_sim.lookouts.empty()) {
                    actor.startPatrol(&m_sim.lookouts, sight * m_scales.sight);
                }
                return id;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}
bool Critters::patrolling(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].patrolling() : false;
}
s32 Critters::lookoutOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].lookout() : -1;
}
void Critters::collect(Combatant& actor) {
    for (const auto& event : actor.takePushes()) {
        m_sim.pushes.push_back(event);
    }
    for (auto& event : actor.takeGrabs()) {
        m_sim.grabs.push_back(event);
    }
    for (auto& event : actor.takeBlows()) {
        m_sim.blows.push_back(event);
    }
    for (auto& event : actor.takeLosses()) {
        m_sim.losses.push_back(std::move(event));
    }
    for (auto& event : actor.takeCues()) {
        m_sim.cues.push_back(std::move(event));
    }
    for (auto& event : actor.takeSpews()) {
        m_sim.spews.push_back(event);
    }
    for (auto& event : actor.takeShots()) {
        m_sim.shots.push_back(event);
    }
    for (const CombatantRam& ram : actor.takeRams()) {
        m_sim.rams.push_back(ram);
    }
}
void Critters::syncFloors() {
    for (auto& actor : m_critters) {
        actor.syncFloor();
    }
}

void Critters::update(s32 ticks, f32 seconds, std::span<const EnemyView> players, bool timeStopped,
                      std::span<const CombatantObstacle> items, Enemies* swarm) {
    syncFloors();
    for (const auto& stock : m_sim.stocks) {
        stock->textures.advance(ticks > 0 ? seconds : 0.0f);
    }
    if (ticks <= 0) {
        return;
    }
    for (auto& actor : m_critters) {
        if (!actor.present()) {
            continue;
        }
        const auto bodies = swarm != nullptr ? swarm->movementBodies() : std::vector<EnemyBody>{};
        actor.setObstacles(items);
        actor.setSwarm(bodies);
        actor.update(ticks, seconds, players, m_critters, timeStopped);
        actor.setObstacles({});
        actor.setSwarm({});
        for (const CombatTrample& trample : actor.takeTramples()) {
            if (swarm == nullptr) {
                continue;
            }
            EnemyHit hit;
            hit.damage = trample.damage;
            hit.flags = trample.flags;
            hit.direction = trample.direction;
            if (trample.hold > 0.0f) {
                // One of its effects, credited to nobody: ProcessEffects' enemy pass passes a
                // body the world's wait holds and holds what it harms over two for the fade.
                swarm->hurtByWorld(trample.enemy, hit, trample.hold, true);
                continue;
            }
            // damage_enemy with no player and no damage type, driven the trample's way.
            hit.where = trample.position;
            hit.close = true;
            swarm->hurt(trample.enemy, hit);
        }
        collect(actor);
    }
}
f32 Critters::hurt(s32 id, const EnemyHit& hit) {
    if (id < 0 || id >= kMost) {
        return 0;
    }
    auto& actor = m_critters[static_cast<usize>(id)];
    const f32 credited = actor.hurt(hit);
    collect(actor);
    return credited;
}
void Critters::freeze(s32 id, s32 ticks) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].freeze(ticks);
    }
}
void Critters::damagedPlayer(s32 id, s32 player, f32 amount) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].damagedPlayer(player, amount);
    }
}
void Critters::blind(s32 id, s32 ticks) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].blind(ticks);
    }
}
void Critters::curb(s32 id, f32 seconds) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].curb(seconds);
    }
}
void Critters::resize(s32 id, f32 scale) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].resize(scale);
    }
}
void Critters::setShrink(f32 scale) {
    for (auto& actor : m_critters) {
        actor.setShrink(scale);
    }
}
void Critters::hold(s32 id, bool held) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].hold(held);
    }
}
void Critters::roar(s32 id) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].roar();
    }
}
std::vector<CombatBlow> Critters::takeBlows() {
    return std::exchange(m_sim.blows, {});
}
std::vector<CombatGrab> Critters::takeGrabs() {
    return std::exchange(m_sim.grabs, {});
}
std::vector<CombatLoss> Critters::takeLosses() {
    return std::exchange(m_sim.losses, {});
}
std::vector<CombatCue> Critters::takeCues() {
    return std::exchange(m_sim.cues, {});
}
std::vector<CombatSpew> Critters::takeSpews() {
    return std::exchange(m_sim.spews, {});
}
std::vector<CombatShot> Critters::takeShots() {
    return std::exchange(m_sim.shots, {});
}
bool Critters::alive(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].alive() : false;
}
bool Critters::dying(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].dying() : false;
}
CombatantKind Critters::kindOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].kind()
                                 : CombatantKind::Unknown;
}
f32 Critters::healthOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].health() : 0;
}
f32 Critters::maxHealthOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].maxHealth() : 1;
}
const Vec3& Critters::positionOf(s32 id) const {
    return m_critters[static_cast<usize>(id)].position();
}
f32 Critters::yawOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].yaw() : 0;
}
f32 Critters::radiusOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].radius() : 0;
}
s32 Critters::targetOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].target() : -1;
}
std::string_view Critters::moveOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].moveName()
                                 : std::string_view{};
}
s32 Critters::moveTypeOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].moveType() : -1;
}
bool Critters::moveDoneOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].moveDone() : false;
}
bool Critters::frozen(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].frozen() : false;
}
bool Critters::blinded(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].blinded() : false;
}
bool Critters::curbed(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].curbed() : false;
}
f32 Critters::scaleOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].scale() : 1;
}
const CritterData* Critters::dataOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].data() : nullptr;
}
std::string Critters::formOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].form() : std::string{};
}
ItemArchive* Critters::archiveOf(s32 id) {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].archive() : nullptr;
}
std::optional<Mat4> Critters::nodeTransformOf(s32 id, std::string_view node) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].nodeTransform(node)
                                 : std::nullopt;
}
std::optional<Mat4> Critters::rootTransformOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].rootTransform()
                                 : std::nullopt;
}
usize Critters::count() const {
    usize count = 0;
    for (const auto& actor : m_critters) {
        count += actor.present() ? 1 : 0;
    }
    return count;
}
std::vector<MissileTarget> Critters::targets(bool solidOnly) const {
    std::vector<MissileTarget> out;
    for (const auto& actor : m_critters) {
        const auto parts = actor.bodyTargets(solidOnly);
        out.insert(out.end(), parts.begin(), parts.end());
    }
    return out;
}
std::optional<s32> Critters::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0;
    for (const auto& actor : m_critters) {
        if (const auto distance = actor.contactDistance(from, to, radius);
            distance.has_value() && (!best.has_value() || *distance < bestDistance)) {
            best = actor.id();
            bestDistance = *distance;
        }
    }
    return best;
}
std::vector<s32> Critters::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (const auto& actor : m_critters) {
        if (actor.within(centre, radius)) {
            out.push_back(actor.id());
        }
    }
    return out;
}
std::vector<s32> Critters::reachedBy(const Vec3& centre, f32 radius, f32 arc,
                                     const Vec3& facing) const {
    std::vector<s32> out;
    for (const auto& actor : m_critters) {
        if (actor.reachedBy(centre, radius, arc, facing)) {
            out.push_back(actor.id());
        }
    }
    return out;
}
void Critters::drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                           const WorldLighting& lighting, f32 presentationAlpha) const {
    for (const auto& actor : m_critters) {
        actor.drawShadow(device, clip, eye, lighting, presentationAlpha);
    }
}

void Critters::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                    const Texture* frozenTexture, const CameraFrame* camera, f32 presentationAlpha,
                    bool healthBars) const {
    for (const auto& actor : m_critters) {
        actor.draw(device, clip, lighting, frozenTexture, camera, m_hitFlash, presentationAlpha,
                   healthBars, actor.id() == m_highlightedId ? m_highlight : nullptr);
    }
}
} // namespace gdl::game
