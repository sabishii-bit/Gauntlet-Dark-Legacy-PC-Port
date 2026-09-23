#include "game/enemies/Critters.h"

#include <cctype>
#include <cmath>
#include <utility>

#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"
namespace gdl::game {
Critters::~Critters() {
    close();
}
void Critters::open(RenderDevice& device, const std::filesystem::path& root,
                    const WorldCollision* collision, const EnemyScales& scales, char realm) {
    close();
    m_device = &device;
    m_root = root;
    m_collision = collision;
    m_scales = scales;
    m_realm = static_cast<char>(std::toupper(static_cast<unsigned char>(realm)));
}
void Critters::close() {
    for (auto& actor : m_critters) {
        actor.clear();
    }
    m_stocks.clear();
    m_blows.clear();
    m_losses.clear();
    m_cues.clear();
    m_spews.clear();
    m_shots.clear();
    m_device = nullptr;
    m_collision = nullptr;
    m_textureFrames = 0;
}
CombatantAssets* Critters::stockFor(const CombatantDefinition& definition) {
    for (auto& stock : m_stocks) {
        if (stock->definition.name == definition.name &&
            stock->definition.kind == definition.kind) {
            return stock.get();
        }
    }
    if (m_device == nullptr) {
        return nullptr;
    }
    auto stock = std::make_unique<CombatantAssets>();
    if (!stock->load(*m_device, m_root, definition, m_realm)) {
        return nullptr;
    }
    m_stocks.push_back(std::move(stock));
    return m_stocks.back().get();
}
std::optional<s32> Critters::spawnGolem(const Vec3& position, f32 yaw) {
    return spawn(Golem::definition(), position, yaw);
}
std::optional<s32> Critters::spawnGeneral(const Vec3& position, f32 yaw) {
    return spawn(General::definition(), position, yaw);
}
std::optional<s32> Critters::spawnGargoyle(const Vec3& position, f32 yaw, std::string_view form) {
    return spawn(Gargoyle::definition(form), position, yaw);
}
std::optional<s32> Critters::spawn(s32 kind, const Vec3& position, f32 yaw, std::string_view form) {
    switch (kind) {
    case kGolemCritter: return spawnGolem(position, yaw);
    case kGeneralCritter: return spawnGeneral(position, yaw);
    case kGargoyleCritter: return spawnGargoyle(position, yaw, form);
    default: return std::nullopt;
    }
}
std::optional<s32> Critters::spawn(const CombatantDefinition& definition, const Vec3& position,
                                   f32 yaw) {
    auto* stock = stockFor(definition);
    if (stock == nullptr) {
        return std::nullopt;
    }
    for (s32 id = 0; id < kMost; ++id) {
        auto& actor = m_critters[static_cast<usize>(id)];
        if (!actor.present()) {
            if (actor.spawn(*stock, id, position, yaw, m_collision, m_scales, m_realm)) {
                return id;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}
void Critters::collect(Combatant& actor) {
    for (auto& event : actor.takeBlows()) {
        m_blows.push_back(event);
    }
    for (auto& event : actor.takeLosses()) {
        m_losses.push_back(std::move(event));
    }
    for (auto& event : actor.takeCues()) {
        m_cues.push_back(std::move(event));
    }
    for (auto& event : actor.takeSpews()) {
        m_spews.push_back(event);
    }
    for (auto& event : actor.takeShots()) {
        m_shots.push_back(event);
    }
}
void Critters::update(s32 ticks, f32 seconds, std::span<const EnemyView> players) {
    if (ticks <= 0) {
        return;
    }
    m_textureFrames += seconds * AnimationPlayer::kDefaultRate;
    const auto frames = static_cast<u32>(std::floor(m_textureFrames));
    m_textureFrames -= static_cast<f32>(frames);
    for (const auto& stock : m_stocks) {
        stock->textures.step(frames);
    }
    for (auto& actor : m_critters) {
        actor.update(ticks, seconds, players, m_critters);
        collect(actor);
    }
}
void Critters::hurt(s32 id, const EnemyHit& hit) {
    if (id < 0 || id >= kMost) {
        return;
    }
    auto& actor = m_critters[static_cast<usize>(id)];
    actor.hurt(hit);
    collect(actor);
}
void Critters::freeze(s32 id, s32 ticks) {
    if (id >= 0 && id < kMost) {
        m_critters[static_cast<usize>(id)].freeze(ticks);
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
    return std::exchange(m_blows, {});
}
std::vector<CombatLoss> Critters::takeLosses() {
    return std::exchange(m_losses, {});
}
std::vector<CombatCue> Critters::takeCues() {
    return std::exchange(m_cues, {});
}
std::vector<CombatSpew> Critters::takeSpews() {
    return std::exchange(m_spews, {});
}
std::vector<CombatShot> Critters::takeShots() {
    return std::exchange(m_shots, {});
}
bool Critters::alive(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].alive() : false;
}
bool Critters::dying(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].dying() : false;
}
s32 Critters::kindOf(s32 id) const {
    return id >= 0 && id < kMost ? m_critters[static_cast<usize>(id)].kind() : 0;
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
std::vector<MissileTarget> Critters::targets() const {
    std::vector<MissileTarget> out;
    for (const auto& actor : m_critters) {
        if (actor.alive()) {
            out.push_back({actor.id(), actor.position(), actor.radius(), 8.0f});
        }
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
void Critters::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                    const Texture* frozenTexture) const {
    for (const auto& actor : m_critters) {
        actor.draw(device, clip, lighting, frozenTexture);
    }
}
} // namespace gdl::game
