#include "game/bosses/Bosses.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <type_traits>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/level/BossKinds.h"
#include "game/level/LevelTriggers.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {
namespace {
void appendConfiguration(NativeStateDigest& digest, const WorldCollision* collision,
                         const EnemyScales& scales, char realm, bool open,
                         const std::optional<LegendProfile>& profile) {
    // Immutable during one binding, but relevant before the first fighter is
    // born. Reopening changes the local epoch and invalidates saved values.
    digest.resource(collision);
    digest.values(scales, realm, open);
    digest.flag(profile.has_value());
    if (profile) {
        digest.fields<13>(*profile);
    }
}

/** How far along the ground the farthest standing player is from `at`, nought for nobody: the
 * original's target records measure with the rise zeroed (CritterCalcTarget 0x80037338-0x80037340,
 * DC 0x8c08c95a, N64 Boss.bin 0xe0886240) and the wake takes their largest (0x80039c20). */
f32 farthestAlongGround(const Vec3& at, std::span<const EnemyView> players) {
    f32 farthest = 0.0f;
    for (const EnemyView& view : players) {
        if (!view.hidden) {
            farthest = std::max(farthest,
                                glm::length(Vec2{view.position.x - at.x, view.position.z - at.z}));
        }
    }
    return farthest;
}

/** Whether a party whose farthest player stands `farthest` away wakes a boss of `threshold`:
 * strictly inside it, or at once for a threshold of nought or under (CritterBossAI
 * 0x80039c50-0x80039c64, DC 0x8c08ac96-0x8c08ac9e, N64 0xe08843e8-0xe0884400). A farthest of
 * nought stands for nobody, whom the original counts 1e21 away (0x80039c4c). */
bool partyWakes(f32 farthest, f32 threshold) {
    return threshold <= 0.0f || (farthest > 0.0f && farthest < threshold);
}

/** The root CritterInitGeo stands a type of `floorOffset` at over `placement`: on the floor its
 * probe finds there, else at the placement's own height, raised by the offset
 * (0x8003e628-0x8003e6b4; DC 0x8c087866-0x8c087932). */
Vec3 rootOver(const WorldCollision* collision, const Vec3& placement, f32 floorOffset) {
    Vec3 root = placement;
    if (collision != nullptr) {
        if (const auto floor = Combatant::bossFloorUnder(*collision, placement)) {
            root.y = floor->y;
        }
    }
    root.y += floorOffset;
    return root;
}
} // namespace

void Bosses::appendSimulationDigest(
    NativeStateDigest& digest, const Simulation& state,
    std::optional<std::span<const TextureAnimator::Clock>> savedClocks) {
    // The same fourteen actual values are copied into local history. Assets
    // identify retained bindings; their mutable clocks are captured separately.
    visitNativeFields<14>(state, [&]<class... Fields>(const Fields&... fields) {
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

bool Bosses::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("Bosses.native.1");
    appendConfiguration(digest, m_collision, m_scales, m_realm, m_device != nullptr,
                        m_legendProfile);
    appendSimulationDigest(digest, m_sim);
    return m_fighter.appendNativeDigest(digest) && digest.good();
}

bool Bosses::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("Bosses.native.1");
    appendConfiguration(digest, m_collision, m_scales, m_realm, m_device != nullptr,
                        m_legendProfile);
    appendSimulationDigest(digest, saved.m_state,
                           std::span<const TextureAnimator::Clock>{saved.m_clocks});
    return m_fighter.appendLocalDigest(digest, saved.m_fighter) && digest.good();
}

bool Bosses::registerNativeResources(NativeStateResources& resources,
                                     std::string_view stablePrefix) const {
    // Discarding a speculative encounter removes it from m_sim.assets, not
    // from history or the immutable cache its older snapshots still borrow.
    for (const auto& stock : m_assetCache) {
        if (!resources.registerCombatantAssets(*stock,
                                               std::format("{}/{}/{}", stablePrefix,
                                                           static_cast<s32>(stock->definition.kind),
                                                           stock->definition.name))) {
            return false;
        }
    }
    return resources.good();
}

bool Bosses::saveLocal(LocalSnapshot& snapshot) const {
    snapshot.m_owner.reset();
    if (!m_fighter.saveLocal(snapshot.m_fighter)) {
        return false;
    }
    snapshot.m_state = m_sim;
    snapshot.m_epoch = m_localEpoch;
    snapshot.m_clocks.resize(m_sim.assets.size());
    snapshot.m_assets.resize(m_sim.assets.size());
    for (usize i = 0; i < m_sim.assets.size(); ++i) {
        snapshot.m_clocks[i] = m_sim.assets[i]->textures.clock();
        snapshot.m_assets[i] = m_sim.assets[i]->m_localBinding;
    }
    snapshot.m_owner = m_localOwner;
    return true;
}
bool Bosses::canRestoreLocal(const LocalSnapshot& snapshot) const {
    return snapshot.m_owner.lock() == m_localOwner && snapshot.m_epoch == m_localEpoch &&
           snapshot.m_clocks.size() == snapshot.m_state.assets.size() &&
           snapshot.m_assets.size() == snapshot.m_state.assets.size() &&
           std::ranges::none_of(snapshot.m_assets,
                                [](const auto& asset) { return asset.expired(); }) &&
           m_fighter.canRestoreLocal(snapshot.m_fighter);
}
bool Bosses::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    if (!canRestoreLocal(left) || !canRestoreLocal(right)) {
        return false;
    }
    bool equal = true;
    const auto same = []<class T>(const T& a, const T& b) {
        if constexpr (std::is_same_v<T, LegendRite>) {
            return a.equalLocal(b);
        } else {
            return a == b;
        }
    };
    visitNativeFields<14>(left.m_state, [&](const auto&... first) {
        visitNativeFields<14>(right.m_state,
                              [&](const auto&... second) { equal = (same(first, second) && ...); });
    });
    return equal && left.m_clocks == right.m_clocks &&
           m_fighter.equalLocal(left.m_fighter, right.m_fighter);
}
bool Bosses::restoreLocal(const LocalSnapshot& snapshot) {
    if (!canRestoreLocal(snapshot)) {
        return false;
    }
    // The same scene still owns every resource and actor. No native action,
    // legend callback or reward publication is run while restoring values.
    m_sim = snapshot.m_state;
    for (usize i = 0; i < m_sim.assets.size(); ++i) {
        m_sim.assets[i]->textures.restoreClock(snapshot.m_clocks[i]);
    }
    return m_fighter.restoreLocal(snapshot.m_fighter);
}

void Bosses::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                  const WorldCollision* collision, const EnemyScales& scales, char realm,
                  std::span<TextureSet* const> textureLenders, const LegendProfile* legendProfile) {
    close();
    m_device = &device;
    m_root = unpackedRoot;
    m_textureLenders.assign(textureLenders.begin(), textureLenders.end());
    m_collision = collision;
    m_scales = scales;
    m_realm = static_cast<char>(std::toupper(static_cast<unsigned char>(realm)));
    if (legendProfile) {
        m_legendProfile = *legendProfile;
    }
}

void Bosses::close() {
    ++m_localEpoch;
    m_fighter.clear();
    m_sim.assets.clear();
    m_assetCache.clear();
    m_textureLenders.clear();
    m_legendProfile.reset();
    m_hitFlash = nullptr;
    m_device = nullptr;
    m_collision = nullptr;
    m_sim.id.reset();
    m_sim.spawnRoot.reset();
    m_sim.defeat.reset();
    m_sim.keyOffset = Vec3{0};
    m_sim.kind = -1;
    m_sim.name.clear();
    m_sim.awake = false;
    m_sim.wakeDistance = 0.0f;
    m_sim.wakeClock = 0.0f;
    m_sim.rite.clear();
    m_sim.roarAsked = false;
    m_sim.legendStruck = false;
    m_sim.legendEvents.clear();
}

std::vector<CombatantAssets*> Bosses::resources() {
    std::vector<CombatantAssets*> result;
    result.reserve(m_sim.assets.size());
    for (auto& stock : m_sim.assets) {
        result.push_back(stock);
    }
    return result;
}
std::vector<const CombatantAssets*> Bosses::resources() const {
    std::vector<const CombatantAssets*> result;
    result.reserve(m_sim.assets.size());
    for (const auto& stock : m_sim.assets) {
        result.push_back(stock);
    }
    return result;
}
bool Bosses::bringLegend(s32 player) {
    const LegendWeakness* weakness = legendWeaknessOf(m_sim.kind);
    if (!m_sim.id.has_value() || weakness == nullptr ||
        m_sim.rite.stage() != LegendRite::Stage::None) {
        return false;
    }
    m_sim.rite.begin(player, *weakness);
    m_sim.roarAsked = false;
    m_fighter.hold(true); // it keeps to its stance while the item is raised
    return true;
}

/** The rite goes by what the boss is doing: risen once its start is over, roared once its
 * roar is; each cue is acted on and kept for the game. */
void Bosses::stageLegend(s32 ticks) {
    if (!m_sim.rite.running() || !m_sim.id.has_value()) {
        return;
    }
    const s32 moveType = m_fighter.moveType();
    const bool risen = m_sim.awake && moveType >= 0 && moveType != MoveDefinition::kStart;
    const CritterData* data = m_fighter.data();
    const bool canRoar = data != nullptr && data->moveOfType(MoveDefinition::kRoar).has_value();
    const bool roarDone = m_sim.roarAsked &&
                          (!canRoar || (moveType == MoveDefinition::kRoar && m_fighter.moveDone()));
    for (const LegendCue cue : m_sim.rite.update(ticks, risen, roarDone)) {
        if (cue == LegendCue::Thrown && LegendShow::strikeOf(m_sim.kind) == LegendStrike::OnThrow) {
            strikeWithLegend();
        } else if (cue == LegendCue::WornOff) {
            m_fighter.curb(0.0f);
            if (LegendShow::restoresSight(m_sim.kind)) {
                // world_update (0x8005674c) clears the initial 18000-tick
                // blindness 29 seconds after the roar, not five minutes later.
                m_fighter.blind(0);
                if (!m_legendProfile || !m_legendProfile->eyeReplacement.empty()) {
                    m_fighter.replaceNodeModel(*m_device, "EYEBALL", {});
                }
            }
        }
        m_sim.legendEvents.push_back(LegendEvent{cue, m_sim.rite.player(), legendRealm()});
    }
    if (m_sim.rite.wantsRoar() && !m_sim.roarAsked) {
        m_sim.roarAsked = true;
        m_fighter.hold(false);
        m_fighter.roar();
    }
}

void Bosses::landLegend() {
    if (LegendShow::strikeOf(m_sim.kind) == LegendStrike::OnLanding) {
        // Scimitar and javelin act on impact; Bellows and Savior on cast release.
        // The roar controls lighting independently of that notification.
        if (m_sim.id.has_value() && m_sim.rite.thrown() && !m_sim.legendStruck &&
            (!LegendShow::landsWhileRiteRuns(m_sim.kind) || m_sim.rite.running())) {
            strikeWithLegend();
            m_sim.legendStruck = true;
        }
        return;
    }
    if (!m_sim.id.has_value() || !m_sim.rite.finishOnImpact()) {
        return;
    }
    strikeWithLegend();
    m_fighter.hold(false);
}

/** The item lands: a share of its health goes at once, and its weakness is put on it. */
void Bosses::strikeWithLegend() {
    const LegendWeakness* weakness = m_sim.rite.weakness();
    if (weakness == nullptr || !m_sim.id.has_value() || !m_fighter.alive()) {
        return;
    }
    if (weakness->beheads) {
        // The scimitar targets the second child (lion), with 1.5 times its current health.
        constexpr s32 kLion = 2;
        if (const Combatant* lion = m_fighter.child(kLion); lion != nullptr && lion->alive()) {
            EnemyHit hit;
            hit.damage = 1.5f * lion->health();
            hit.player = m_sim.rite.player();
            m_fighter.hurt(hit, kLion);
        }
    } else if (weakness->harms()) {
        EnemyHit hit;
        hit.damage =
            weakness->damage > 0.0f ? weakness->damage : weakness->healthShare * m_fighter.health();
        hit.player = m_sim.rite.player();
        m_fighter.hurt(hit);
    }
    if (weakness->frozenTicks > 0) {
        m_fighter.freeze(weakness->frozenTicks);
    }
    if (weakness->blindTicks > 0) {
        m_fighter.blind(weakness->blindTicks);
    }
    if (const std::string_view eye = LegendShow::eyeReplacementOf(m_sim.kind); !eye.empty()) {
        const std::string_view replacement =
            m_legendProfile ? std::string_view{m_legendProfile->eyeReplacement} : eye;
        if (!replacement.empty() &&
            !m_fighter.replaceNodeModel(*m_device, "EYEBALL", replacement)) {
            log::warn("Plague javelin: cannot bind {} replacement", replacement);
        }
    }
    if (weakness->curbs()) {
        m_fighter.curb(weakness->curbSeconds);
    }
    m_fighter.resize(weakness->scale);
    if (const std::optional<Color> tint = LegendShow::strikeTintOf(m_sim.kind)) {
        m_fighter.tint(*tint);
    }
}

std::vector<LegendEvent> Bosses::takeLegendEvents() {
    return std::exchange(m_sim.legendEvents, {});
}

bool Bosses::frozen() const {
    return m_sim.id.has_value() && m_fighter.frozen();
}
bool Bosses::blinded() const {
    return m_sim.id.has_value() && m_fighter.blinded();
}
bool Bosses::curbed() const {
    return m_sim.id.has_value() && m_fighter.curbed();
}

CombatantAssets* Bosses::preload(s32 kind, std::unique_ptr<CombatantAssets> assets) {
    const auto name = bossNameOf(kind);
    if (m_device == nullptr || m_sim.id || name.empty() || !assets ||
        assets->definition.name != name || assets->definition.kind != CombatantKind::Boss ||
        assets->data.kind() != CombatantKind::Boss || assets->tree == nullptr ||
        !assets->body.bound() || std::ranges::any_of(m_assetCache, [&](const auto& value) {
            return value->definition.name == name;
        })) {
        return nullptr;
    }
    auto* result = assets.get();
    result->textures.restoreClock({});
    m_assetCache.push_back(std::move(assets));
    m_sim.assets.push_back(result);
    return result;
}

CombatantAssets* Bosses::preload(s32 kind) {
    const std::string_view name = bossNameOf(kind);
    if (name.empty() || m_device == nullptr) {
        return nullptr;
    }
    CombatantAssets* assets = nullptr;
    for (auto& loaded : m_sim.assets) {
        if (loaded->definition.name == name) {
            assets = loaded;
            break;
        }
    }
    if (assets == nullptr) {
        for (const auto& cached : m_assetCache) {
            if (cached->definition.name == name) {
                assets = cached.get();
                break;
            }
        }
        if (assets == nullptr) {
            auto loaded = std::make_unique<CombatantAssets>();
            if (!loaded->load(*m_device, m_root, bossDefinition(name), m_realm, m_textureLenders)) {
                return nullptr;
            }
            assets = loaded.get();
            m_assetCache.push_back(std::move(loaded));
        }
        assets->textures.restoreClock({});
        m_sim.assets.push_back(assets);
    }
    // Keep the javelin's alternate eye in the load-time resource roster.
    if (const std::string_view eye = LegendShow::eyeReplacementOf(kind); !eye.empty()) {
        const std::string_view replacement =
            m_legendProfile ? std::string_view{m_legendProfile->eyeReplacement} : eye;
        if (!replacement.empty()) {
            assets->prepareReplacement(*m_device, "EYEBALL", replacement);
        }
    }
    return assets;
}
bool Bosses::spawn(s32 kind, const Vec3& position, f32 yaw, f32 wakeDistance) {
    if (m_sim.id.has_value()) {
        return false;
    }
    auto* assets = preload(kind);
    if (assets == nullptr) {
        return false;
    }
    if (!m_fighter.spawn(*assets, kTargetId, position, yaw, m_collision, m_scales, m_realm)) {
        return false;
    }
    m_fighter.prepareEntrance();
    m_sim.id = kTargetId;
    m_sim.kind = kind;
    m_sim.name = bossNameOf(kind);
    m_sim.awake = false;
    m_sim.wakeClock = 0.0f;
    const CritterData* data = m_fighter.data();
    m_sim.wakeDistance = wakeDistance;
    m_sim.keyOffset = data != nullptr ? data->keyOffset() : Vec3{0};
    m_sim.spawnRoot = rootOver(m_collision, position, data != nullptr ? data->floorOffset() : 0.0f);
    if (m_sim.wakeDistance <= 0.0f && data != nullptr) {
        m_sim.wakeDistance = data->wakeThreshold();
    }
    return true;
}

void Bosses::update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                    bool activationReady, const LevelTriggers* triggers) {
    for (const auto& assets : m_sim.assets) {
        assets->textures.advance(0);
    }
    if (!m_sim.id.has_value()) {
        return;
    }
    m_fighter.captureEncounterPlayers(players);
    // Wait through the bound family's activation deadline, then for the whole party to come
    // within its threshold. Ordinary GDL retains its two-second delay.
    if (!m_sim.awake) {
        const auto stock = std::ranges::find_if(
            m_sim.assets, [&](const auto* assets) { return &assets->data == m_fighter.data(); });
        const bool decodedEntrance = stock != m_sim.assets.end() && (*stock)->initialMoveBeforeWake;
        const f32 farthest = farthestAlongGround(*position(), players);
        // An imported rite that wakes its boss does so wherever the party stands (N64 Boss.bin
        // 0xe0884424).
        const bool riteWakes =
            m_legendProfile && m_legendProfile->wakesOnDeadline && m_sim.rite.running();
        const f32 delay = stock != m_sim.assets.end() ? (*stock)->wakeDelay : kWakeArmSeconds;
        // The test reads a deadline an earlier update set (CritterBossAI 0x80039bf0): the clock
        // as the last update left it.
        if (m_sim.wakeClock > 0 && m_sim.wakeClock >= delay &&
            (riteWakes || partyWakes(farthest, m_sim.wakeDistance))) {
            wake();
        }
        if (!m_sim.awake) {
            if (ticks > 0 && decodedEntrance) {
                for (auto* assets : m_sim.assets) {
                    assets->textures.advance(seconds);
                }
                m_fighter.advanceEntrance(seconds);
            }
            // CritterBossAI's wake test (0x80039bac) comes before its step (CritterTranslate,
            // 0x8003a218), which holds it to its territory on INIT's zero pace, and before its
            // floor probe (0x8003a2d4), so the party was measured from where the last update
            // stood it.
            if (ticks > 0) {
                m_fighter.holdTerritory();
            }
            const std::optional<FloorHit> support = m_fighter.settleOnFloor();
            advanceWakeClock(seconds, deadlineMayStart(activationReady, triggers, support));
            return;
        }
    }
    // CritterDamage exempts the opening states 1..4, not state 5 while the
    // weakness is wearing off. Ordinary party damage scaling resumes after the roar.
    m_fighter.takeFullHarm(m_sim.rite.stage() == LegendRite::Stage::Carried ||
                           m_sim.rite.darkens());
    if (ticks > 0) {
        for (const auto& assets : m_sim.assets) {
            assets->textures.advance(seconds);
        }
    }
    // It falls where the last update stood it: the original removes it before its floor probe
    // (CritterDelInst at 0x8003a0c4), so its defeat is taken before the fighter's update.
    const Vec3 before = m_fighter.position();
    m_fighter.update(ticks, seconds, players);
    stageLegend(ticks);
    // The rite runs on through the death throes until gBossDead, when world_update ends it
    // (state 99, 0x80056d04-0x80056d14) after its turn of the frame the hold begins in.
    if (m_fighter.deathPlayed()) {
        m_sim.rite.conclude();
    }
    if (!m_fighter.present()) {
        m_sim.defeat = before;
        m_sim.id.reset();
        m_sim.rite.clear();
    }
}

void Bosses::arm(f32 seconds, bool activationReady, const LevelTriggers* triggers) {
    if (!m_sim.awake) {
        advanceWakeClock(seconds, deadlineMayStart(activationReady, triggers, std::nullopt));
    }
}

bool Bosses::supportActivated() const {
    const CritterData* data = m_fighter.data();
    return data != nullptr && (data->typeFlags() & CritterData::kSupportActivated) != 0;
}

bool Bosses::deadlineMayStart(bool activationReady, const LevelTriggers* triggers,
                              const std::optional<FloorHit>& support) const {
    // A type that waits for its floor arms in its floor probe, once the floor found there or
    // that floor's parent has moved on this pass (GC CritterBossAI 0x8003a314-0x8003a398, DC
    // ProcessCritter 0x8c08b786-0x8c08b7c4). An imported level answers for its own floors.
    if (!activationReady || triggers == nullptr || !supportActivated()) {
        return activationReady;
    }
    return support.has_value() && triggers->movesFloor(support->object);
}

void Bosses::advanceWakeClock(f32 seconds, bool mayStart) {
    // Only the level's loading clears a deadline once set (CritterInit 0x80042374): a floor
    // that stops does not disarm it.
    if (m_sim.wakeClock > 0 || mayStart) {
        m_sim.wakeClock += seconds;
    }
}

std::vector<CombatBlow> Bosses::takeBlows() {
    return m_fighter.takeBlows();
}

void Bosses::wake() {
    if (m_sim.id.has_value() && !m_sim.awake) {
        m_fighter.beginEntrance();
        m_sim.awake = true;
    }
}

std::vector<CombatCue> Bosses::takeCues() {
    auto cues = m_fighter.takeCues();
    const std::string_view ownStump = LegendShow::lionStumpOf(m_sim.kind);
    const std::string_view stump =
        m_legendProfile ? std::string_view{m_legendProfile->lionStump} : ownStump;
    if (!ownStump.empty() && m_sim.rite.stage() != LegendRite::Stage::None && !stump.empty()) {
        for (auto& cue : cues) {
            if (cue.tree == "STUMPL") {
                cue.tree = stump;
            }
        }
    }
    return cues;
}

std::optional<Mat4> Bosses::legendTargetTransform() const {
    if (!m_sim.id) {
        return {};
    }
    if (m_legendProfile) {
        if (!m_legendProfile->targetNode) {
            return {};
        }
        for (const auto* stock : m_sim.assets) {
            if (&stock->data == m_fighter.data() && stock->tree &&
                *m_legendProfile->targetNode < stock->tree->nodes.size()) {
                return nodeTransform(stock->tree->nodes[*m_legendProfile->targetNode].name);
            }
        }
        return {};
    }
    if (const std::string_view node = LegendShow::targetNodeOf(m_sim.kind); !node.empty()) {
        return nodeTransform(node);
    }
    return {};
}

std::optional<Mat4> Bosses::nodeTransform(std::string_view node) const {
    return m_sim.id.has_value() ? m_fighter.nodeTransform(node) : std::nullopt;
}

std::optional<Mat4> Bosses::rootTransform() const {
    return m_sim.id.has_value() ? m_fighter.rootTransform() : std::nullopt;
}

std::vector<CombatLoss> Bosses::takeLosses() {
    return m_fighter.takeLosses();
}

std::optional<Vec3> Bosses::takeDefeat() {
    return std::exchange(m_sim.defeat, std::nullopt);
}

f32 Bosses::hurt(const EnemyHit& hit, s32 partId) {
    // Asleep it takes nothing and is not woken by it (CritterDamage: state under two).
    if (m_sim.id.has_value() && m_sim.awake) {
        return m_fighter.hurt(hit, partId);
    }
    return 0;
}

void Bosses::damagedPlayer(s32 player, f32 amount, s32 partId) {
    if (m_sim.id.has_value() && m_sim.awake) {
        m_fighter.damagedPlayer(player, amount, partId);
    }
}

std::vector<MissileTarget> Bosses::targets(bool solidOnly) const {
    // Single-body bosses have animated hit nodes too. Their floor anchor is not
    // a damage volume: Skorne's lies below the arena, well beneath his torso.
    return m_fighter.bodyTargets(solidOnly);
}

std::optional<s32> Bosses::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    return m_fighter.contactDistance(from, to, radius).has_value() ? std::optional{kTargetId}
                                                                   : std::nullopt;
}

bool Bosses::within(const Vec3& centre, f32 radius) const {
    return m_fighter.within(centre, radius);
}

bool Bosses::reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const {
    return m_fighter.reachedBy(centre, radius, arc, facing);
}

void Bosses::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                  const Texture* frozenTexture, f32 presentationAlpha) const {
    m_fighter.draw(device, clip, lighting, frozenTexture, nullptr, m_hitFlash, presentationAlpha);
}

BossView Bosses::view() const {
    BossView view;
    view.kind = m_sim.kind;
    view.name = m_sim.name;
    view.awake = m_sim.awake;
    // BossActivate (0x8001b844) sets gBossActive at the wake, and for a type that waits for
    // its floor already as it arms (0x8003a398).
    view.engaged = m_sim.awake || (supportActivated() && m_sim.wakeClock > 0);
    // CritterBossAI sets gBossDead as the death's hold begins (0x8003a108) and BossDeath as the
    // boss goes (0x8001b8bc); only the level's loading clears it (0x8001bc34, 0x80042378).
    view.deathPlayed = m_sim.id.has_value() ? m_fighter.deathPlayed() : m_sim.awake;
    if (m_sim.id.has_value() && (m_fighter.alive() || m_fighter.dying())) {
        view.health = m_fighter.health();
        view.maxHealth = m_fighter.maxHealth();
        view.alive = m_fighter.alive();
    }
    return view;
}

const HealthMeterDefinition* Bosses::meter() const {
    const CritterData* data = m_sim.id.has_value() ? m_fighter.data() : nullptr;
    return data != nullptr ? &data->meter() : nullptr;
}

ItemArchive* Bosses::archive() {
    return m_sim.id.has_value() ? m_fighter.archive() : nullptr;
}

std::vector<HealthMeterReading> Bosses::healthMeters() const {
    std::vector<HealthMeterReading> readings;
    if (!m_sim.id.has_value()) {
        return readings;
    }
    const auto append = [&readings](const Combatant& fighter) {
        if (const CritterData* data = fighter.data(); data != nullptr && data->meter().shown) {
            readings.push_back({data->meter(), fighter.health(), fighter.maxHealth()});
        }
    };
    append(m_fighter);
    for (usize index = 0; index < m_fighter.childCount(); ++index) {
        if (const Combatant* child = m_fighter.child(kTargetId + 1 + static_cast<s32>(index))) {
            append(*child);
        }
    }
    return readings;
}

const Vec3* Bosses::position() const {
    return m_sim.id.has_value() ? &m_fighter.position() : nullptr;
}

f32 Bosses::facing() const {
    return m_sim.id.has_value() ? m_fighter.yaw() : 0.0f;
}

f32 Bosses::radius() const {
    return m_sim.id.has_value() ? m_fighter.radius() : 0.0f;
}

f32 Bosses::height() const {
    if (!m_sim.id.has_value()) {
        return 0.0f;
    }
    const CritterData* data = m_fighter.data();
    if (data == nullptr) {
        return 0.0f;
    }
    const f32 centre = data->floorOffset() + data->originOffset().y * m_fighter.scale();
    return centre > 0.0f ? centre : data->radius();
}

Vec3 Bosses::cameraOffset() const {
    const CritterData* data = m_sim.id.has_value() ? m_fighter.data() : nullptr;
    return data != nullptr ? Vec3{0.0f, data->floorOffset() + data->vertDrift(), 0.0f} : Vec3{0.0f};
}

std::optional<Vec3> Bosses::keySpot() const {
    if (!m_sim.spawnRoot.has_value()) {
        return std::nullopt;
    }
    return *m_sim.spawnRoot + m_sim.keyOffset;
}

std::string_view Bosses::moveName() const {
    return m_sim.id.has_value() ? m_fighter.moveName() : std::string_view{};
}

} // namespace gdl::game
