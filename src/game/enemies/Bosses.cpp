#include "game/enemies/Bosses.h"

#include <cstdint>
#include <utility>

namespace gdl::game {

void Bosses::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                  const WorldCollision* collision, const EnemyScales& scales, char realm) {
    close();
    m_fighter.open(device, unpackedRoot, collision, scales, realm);
}

void Bosses::close() {
    m_fighter.close();
    m_id.reset();
    m_kind = -1;
    m_name.clear();
    m_awake = false;
    m_wakeDistance = 0.0f;
    m_rite.clear();
    m_roarAsked = false;
    m_legendEvents.clear();
}

bool Bosses::bringLegend(std::int32_t player) {
    const LegendWeakness* weakness = legendWeaknessOf(m_kind);
    if (!m_id.has_value() || weakness == nullptr || m_rite.stage() != LegendRite::Stage::None) {
        return false;
    }
    m_rite.begin(player, *weakness);
    m_roarAsked = false;
    m_fighter.hold(*m_id, true); // it keeps to its stance while the item is raised
    return true;
}

/** The rite goes by what the boss is doing: risen once its start is over, roared once its
 * roar is; each cue is acted on and kept for the game. */
void Bosses::stageLegend(std::int32_t ticks) {
    if (!m_rite.running() || !m_id.has_value()) {
        return;
    }
    const std::int32_t moveType = m_fighter.moveTypeOf(*m_id);
    const bool risen = m_awake && moveType >= 0 && moveType != CritterMove::kStart;
    const CritterData* data = m_fighter.dataOf(*m_id);
    const bool canRoar = data != nullptr && data->moveOfType(CritterMove::kRoar).has_value();
    const bool roarDone =
        m_roarAsked &&
        (!canRoar || (moveType == CritterMove::kRoar && m_fighter.moveDoneOf(*m_id)));
    for (const LegendCue cue : m_rite.update(ticks, risen, roarDone)) {
        if (cue == LegendCue::Thrown && m_kind != 34) {
            strikeWithLegend();
        } else if (cue == LegendCue::WornOff) {
            m_fighter.curb(*m_id, 0.0f);
        }
        m_legendEvents.push_back(LegendEvent{cue, m_rite.player(), legendRealm()});
    }
    if (m_rite.wantsRoar() && !m_roarAsked) {
        m_roarAsked = true;
        m_fighter.hold(*m_id, false);
        m_fighter.roar(*m_id);
    }
}

void Bosses::landLegend() {
    if (!m_id.has_value() || !m_rite.finishOnImpact()) {
        return;
    }
    strikeWithLegend();
    m_fighter.hold(*m_id, false);
}

/** The item lands: a share of its health goes at once, and its weakness is put on it. */
void Bosses::strikeWithLegend() {
    const LegendWeakness* weakness = m_rite.weakness();
    if (weakness == nullptr || !m_id.has_value() || !m_fighter.alive(*m_id)) {
        return;
    }
    if (weakness->harms()) {
        EnemyHit hit;
        hit.damage = weakness->damage > 0.0f ? weakness->damage
                                             : weakness->healthShare * m_fighter.healthOf(*m_id);
        hit.player = m_rite.player();
        m_fighter.hurt(*m_id, hit);
    }
    if (weakness->frozenTicks > 0) {
        m_fighter.freeze(*m_id, weakness->frozenTicks);
    }
    if (weakness->blindTicks > 0) {
        m_fighter.blind(*m_id, weakness->blindTicks);
    }
    if (weakness->curbs()) {
        m_fighter.curb(*m_id, weakness->curbSeconds);
    }
    m_fighter.resize(*m_id, weakness->scale);
}

std::vector<LegendEvent> Bosses::takeLegendEvents() {
    return std::exchange(m_legendEvents, {});
}

bool Bosses::frozen() const {
    return m_id.has_value() && m_fighter.frozen(*m_id);
}
bool Bosses::blinded() const {
    return m_id.has_value() && m_fighter.blinded(*m_id);
}
bool Bosses::curbed() const {
    return m_id.has_value() && m_fighter.curbed(*m_id);
}

bool Bosses::spawn(std::int32_t kind, const Vec3& position, float yaw, float wakeDistance) {
    const std::string_view name = bossNameOf(kind);
    if (name.empty() || m_id.has_value()) {
        return false;
    }
    m_id = m_fighter.spawn(kBossCritter, position, yaw, name);
    if (!m_id.has_value()) {
        return false;
    }
    m_kind = kind;
    m_name = name;
    m_awake = false;
    const CritterData* data = m_fighter.dataOf(*m_id);
    m_wakeDistance = wakeDistance;
    if (m_wakeDistance <= 0.0f && data != nullptr) {
        m_wakeDistance = data->wakeThreshold();
    }
    return true;
}

void Bosses::update(std::int32_t ticks, float seconds, std::span<const EnemyView> players) {
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
    m_fighter.update(ticks, seconds, players);
    stageLegend(ticks);
    if (m_fighter.count() == 0) {
        m_id.reset();
        m_rite.clear();
    }
}

std::vector<CritterBlow> Bosses::takeBlows() {
    return m_fighter.takeBlows();
}

std::optional<Mat4> Bosses::nodeTransform(std::string_view node) const {
    return m_id.has_value() ? m_fighter.nodeTransformOf(*m_id, node) : std::nullopt;
}

std::vector<CritterLoss> Bosses::takeLosses() {
    return m_fighter.takeLosses();
}

void Bosses::hurt(const EnemyHit& hit) {
    if (m_id.has_value()) {
        m_awake = true; // struck, it wakes
        m_fighter.hurt(*m_id, hit);
    }
}

std::vector<MissileTarget> Bosses::targets() const {
    return m_fighter.targets();
}

std::optional<std::int32_t> Bosses::struckBy(const Vec3& from, const Vec3& to, float radius) const {
    return m_fighter.struckBy(from, to, radius);
}

bool Bosses::within(const Vec3& centre, float radius) const {
    return !m_fighter.within(centre, radius).empty();
}

bool Bosses::reachedBy(const Vec3& centre, float radius, float arc, const Vec3& facing) const {
    return !m_fighter.reachedBy(centre, radius, arc, facing).empty();
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
    if (m_id.has_value() && (m_fighter.alive(*m_id) || m_fighter.dying(*m_id))) {
        view.health = m_fighter.healthOf(*m_id);
        view.maxHealth = m_fighter.maxHealthOf(*m_id);
        view.alive = m_fighter.alive(*m_id);
    }
    return view;
}

const CritterMeter* Bosses::meter() const {
    const CritterData* data = m_id.has_value() ? m_fighter.dataOf(*m_id) : nullptr;
    return data != nullptr ? &data->meter() : nullptr;
}

ItemArchive* Bosses::archive() {
    return m_id.has_value() ? m_fighter.archiveOf(*m_id) : nullptr;
}

const Vec3* Bosses::position() const {
    return m_id.has_value() ? &m_fighter.positionOf(*m_id) : nullptr;
}

float Bosses::facing() const {
    return m_id.has_value() ? m_fighter.yawOf(*m_id) : 0.0f;
}

float Bosses::radius() const {
    return m_id.has_value() ? m_fighter.radiusOf(*m_id) : 0.0f;
}

float Bosses::height() const {
    if (!m_id.has_value()) {
        return 0.0f;
    }
    const CritterData* data = m_fighter.dataOf(*m_id);
    if (data == nullptr) {
        return 0.0f;
    }
    const float centre = data->floorOffset() + data->originOffset().y * m_fighter.scaleOf(*m_id);
    return centre > 0.0f ? centre : data->radius();
}

Vec3 Bosses::cameraOffset() const {
    const CritterData* data = m_id.has_value() ? m_fighter.dataOf(*m_id) : nullptr;
    return data != nullptr ? Vec3{0.0f, data->floorOffset() + data->vertDrift(), 0.0f} : Vec3{0.0f};
}

std::string_view Bosses::moveName() const {
    return m_id.has_value() ? m_fighter.moveOf(*m_id) : std::string_view{};
}

} // namespace gdl::game
