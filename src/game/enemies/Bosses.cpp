#include "game/enemies/Bosses.h"

#include <cctype>
#include <cmath>
#include <memory>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {

void Bosses::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                  const WorldCollision* collision, const EnemyScales& scales, char realm) {
    close();
    m_device = &device;
    m_root = unpackedRoot;
    m_collision = collision;
    m_scales = scales;
    m_realm = static_cast<char>(std::toupper(static_cast<unsigned char>(realm)));
}

void Bosses::close() {
    m_fighter.clear();
    m_assets.clear();
    m_device = nullptr;
    m_collision = nullptr;
    m_textureFrames = 0;
    m_id.reset();
    m_cameraBase.reset();
    m_defeat.reset();
    m_rewardOffset = Vec3{0};
    m_kind = -1;
    m_name.clear();
    m_awake = false;
    m_wakeDistance = 0.0f;
    m_rite.clear();
    m_roarAsked = false;
    m_legendStruck = false;
    m_legendEvents.clear();
}

bool Bosses::bringLegend(s32 player) {
    const LegendWeakness* weakness = legendWeaknessOf(m_kind);
    if (!m_id.has_value() || weakness == nullptr || m_rite.stage() != LegendRite::Stage::None) {
        return false;
    }
    m_rite.begin(player, *weakness);
    m_roarAsked = false;
    m_fighter.hold(true); // it keeps to its stance while the item is raised
    return true;
}

/** The rite goes by what the boss is doing: risen once its start is over, roared once its
 * roar is; each cue is acted on and kept for the game. */
void Bosses::stageLegend(s32 ticks) {
    if (!m_rite.running() || !m_id.has_value()) {
        return;
    }
    const s32 moveType = m_fighter.moveType();
    const bool risen = m_awake && moveType >= 0 && moveType != MoveDefinition::kStart;
    const CritterData* data = m_fighter.data();
    const bool canRoar = data != nullptr && data->moveOfType(MoveDefinition::kRoar).has_value();
    const bool roarDone =
        m_roarAsked && (!canRoar || (moveType == MoveDefinition::kRoar && m_fighter.moveDone()));
    for (const LegendCue cue : m_rite.update(ticks, risen, roarDone)) {
        if (cue == LegendCue::Thrown && m_kind != 34 && m_kind != 35 && m_kind != 36 &&
            m_kind != 37 && m_kind != 42) {
            strikeWithLegend();
        } else if (cue == LegendCue::WornOff) {
            m_fighter.curb(0.0f);
        }
        m_legendEvents.push_back(LegendEvent{cue, m_rite.player(), legendRealm()});
    }
    if (m_rite.wantsRoar() && !m_roarAsked) {
        m_roarAsked = true;
        m_fighter.hold(false);
        m_fighter.roar();
    }
}

void Bosses::landLegend() {
    if (m_kind == 35 || m_kind == 37 || m_kind == 42) {
        // The scimitar acts on impact; Bellows and Savior act on cast release.
        // The roar controls lighting independently of that notification.
        if (m_id.has_value() && m_rite.thrown() && !m_legendStruck) {
            strikeWithLegend();
            m_legendStruck = true;
        }
        return;
    }
    if (!m_id.has_value() || !m_rite.finishOnImpact()) {
        return;
    }
    strikeWithLegend();
    m_fighter.hold(false);
}

/** The item lands: a share of its health goes at once, and its weakness is put on it. */
void Bosses::strikeWithLegend() {
    const LegendWeakness* weakness = m_rite.weakness();
    if (weakness == nullptr || !m_id.has_value() || !m_fighter.alive()) {
        return;
    }
    if (weakness->beheads) {
        // The scimitar targets the second child (lion), with 1.5 times its current health.
        constexpr s32 kLion = 2;
        if (const Combatant* lion = m_fighter.child(kLion); lion != nullptr && lion->alive()) {
            EnemyHit hit;
            hit.damage = 1.5f * lion->health();
            hit.player = m_rite.player();
            m_fighter.hurt(hit, kLion);
        }
    } else if (weakness->harms()) {
        EnemyHit hit;
        hit.damage =
            weakness->damage > 0.0f ? weakness->damage : weakness->healthShare * m_fighter.health();
        hit.player = m_rite.player();
        m_fighter.hurt(hit);
    }
    if (weakness->frozenTicks > 0) {
        m_fighter.freeze(weakness->frozenTicks);
    }
    if (weakness->blindTicks > 0) {
        m_fighter.blind(weakness->blindTicks);
    }
    if (weakness->curbs()) {
        m_fighter.curb(weakness->curbSeconds);
    }
    m_fighter.resize(weakness->scale);
    if (m_kind == 37) {
        m_fighter.tint(Color::rgba(64, 255, 64));
    }
}

std::vector<LegendEvent> Bosses::takeLegendEvents() {
    return std::exchange(m_legendEvents, {});
}

bool Bosses::frozen() const {
    return m_id.has_value() && m_fighter.frozen();
}
bool Bosses::blinded() const {
    return m_id.has_value() && m_fighter.blinded();
}
bool Bosses::curbed() const {
    return m_id.has_value() && m_fighter.curbed();
}

bool Bosses::spawn(s32 kind, const Vec3& position, f32 yaw, f32 wakeDistance) {
    const std::string_view name = bossNameOf(kind);
    if (name.empty() || m_id.has_value()) {
        return false;
    }
    if (m_device == nullptr) {
        return false;
    }
    CombatantAssets* assets = nullptr;
    for (auto& loaded : m_assets) {
        if (loaded->definition.name == name) {
            assets = loaded.get();
            break;
        }
    }
    if (assets == nullptr) {
        auto loaded = std::make_unique<CombatantAssets>();
        if (!loaded->load(*m_device, m_root, bossDefinition(name), m_realm)) {
            return false;
        }
        assets = loaded.get();
        m_assets.push_back(std::move(loaded));
    }
    if (!m_fighter.spawn(*assets, kTargetId, position, yaw, m_collision, m_scales, m_realm)) {
        return false;
    }
    m_id = kTargetId;
    m_kind = kind;
    m_name = name;
    m_awake = false;
    const CritterData* data = m_fighter.data();
    m_wakeDistance = wakeDistance;
    m_rewardOffset = rewardOffset();
    m_cameraBase =
        m_fighter.position() + Vec3{0.0f, data != nullptr ? data->floorOffset() : 0.0f, 0.0f};
    if (m_wakeDistance <= 0.0f && data != nullptr) {
        m_wakeDistance = data->wakeThreshold();
    }
    return true;
}

void Bosses::update(s32 ticks, f32 seconds, std::span<const EnemyView> players) {
    if (!m_id.has_value()) {
        return;
    }
    // Asleep until the party comes within its threshold; then it fights on until it falls.
    if (!m_awake) {
        const Vec3* at = position();
        for (const EnemyView& view : players) {
            if (at != nullptr && !view.hidden &&
                (m_wakeDistance <= 0.0f || glm::distance(view.position, *at) <= m_wakeDistance)) {
                m_awake = true;
            }
        }
        if (!m_awake) {
            return;
        }
    }
    if (ticks > 0) {
        m_textureFrames += seconds * AnimationPlayer::kDefaultRate;
        const auto frames = static_cast<u32>(std::floor(m_textureFrames));
        m_textureFrames -= static_cast<f32>(frames);
        for (const auto& assets : m_assets) {
            assets->textures.step(frames);
        }
    }
    const Vec3 before = m_fighter.position();
    m_fighter.update(ticks, seconds, players);
    stageLegend(ticks);
    if (!m_fighter.present()) {
        m_defeat = before;
        m_id.reset();
        m_rite.clear();
    }
}

std::vector<CombatBlow> Bosses::takeBlows() {
    return m_fighter.takeBlows();
}

std::vector<CombatCue> Bosses::takeCues() {
    auto cues = m_fighter.takeCues();
    if (m_kind == 35 && m_rite.stage() != LegendRite::Stage::None) {
        for (auto& cue : cues) {
            if (cue.tree == "STUMPL") {
                cue.tree = "STUMPLQ";
            }
        }
    }
    return cues;
}

std::optional<Mat4> Bosses::nodeTransform(std::string_view node) const {
    return m_id.has_value() ? m_fighter.nodeTransform(node) : std::nullopt;
}

std::optional<Mat4> Bosses::rootTransform() const {
    return m_id.has_value() ? m_fighter.rootTransform() : std::nullopt;
}

std::vector<CombatLoss> Bosses::takeLosses() {
    return m_fighter.takeLosses();
}

std::optional<Vec3> Bosses::takeDefeat() {
    return std::exchange(m_defeat, std::nullopt);
}

void Bosses::hurt(const EnemyHit& hit, s32 partId) {
    if (m_id.has_value()) {
        m_awake = true; // struck, it wakes
        m_fighter.hurt(hit, partId);
        if (!m_fighter.alive()) {
            m_rite.clear();
            m_fighter.hold(false);
            m_legendEvents.clear();
        }
    }
}

std::vector<MissileTarget> Bosses::targets() const {
    if (m_fighter.childCount() > 0) {
        return m_fighter.bodyTargets();
    }
    return m_fighter.alive() ? std::vector<MissileTarget>{{kTargetId, m_fighter.position(),
                                                           m_fighter.radius(), 8.0f}}
                             : std::vector<MissileTarget>{};
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
                  const Texture* frozenTexture) const {
    m_fighter.draw(device, clip, lighting, frozenTexture);
}

BossView Bosses::view() const {
    BossView view;
    view.kind = m_kind;
    view.name = m_name;
    view.awake = m_awake;
    if (m_id.has_value() && (m_fighter.alive() || m_fighter.dying())) {
        view.health = m_fighter.health();
        view.maxHealth = m_fighter.maxHealth();
        view.alive = m_fighter.alive();
    }
    return view;
}

const HealthMeterDefinition* Bosses::meter() const {
    const CritterData* data = m_id.has_value() ? m_fighter.data() : nullptr;
    return data != nullptr ? &data->meter() : nullptr;
}

ItemArchive* Bosses::archive() {
    return m_id.has_value() ? m_fighter.archive() : nullptr;
}

std::vector<HealthMeterReading> Bosses::healthMeters() const {
    std::vector<HealthMeterReading> readings;
    if (!m_id.has_value()) {
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
    return m_id.has_value() ? &m_fighter.position() : nullptr;
}

f32 Bosses::facing() const {
    return m_id.has_value() ? m_fighter.yaw() : 0.0f;
}

f32 Bosses::radius() const {
    return m_id.has_value() ? m_fighter.radius() : 0.0f;
}

f32 Bosses::height() const {
    if (!m_id.has_value()) {
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
    const CritterData* data = m_id.has_value() ? m_fighter.data() : nullptr;
    return data != nullptr ? Vec3{0.0f, data->floorOffset() + data->vertDrift(), 0.0f} : Vec3{0.0f};
}

Vec3 Bosses::rewardOffset() const {
    const CritterData* data = m_id.has_value() ? m_fighter.data() : nullptr;
    return data != nullptr ? data->originOffset() + Vec3{0, data->floorOffset(), 0}
                           : m_rewardOffset;
}

std::string_view Bosses::moveName() const {
    return m_id.has_value() ? m_fighter.moveName() : std::string_view{};
}

} // namespace gdl::game
