#include "game/enemies/Critters.h"

#include <cctype>
#include <utility>

#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"
namespace gdl::game {
Critters::~Critters() {
    close();
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
    for (auto& actor : m_critters) {
        actor.clear();
    }
    m_stocks.clear();
    m_textureLenders.clear();
    m_gargoyleForm.clear();
    m_blows.clear();
    m_grabs.clear();
    m_losses.clear();
    m_cues.clear();
    m_spews.clear();
    m_shots.clear();
    m_rams.clear();
    m_pushes.clear();
    m_hitFlash = nullptr;
    m_device = nullptr;
    m_collision = nullptr;
    m_hazards = nullptr;
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
    if (!stock->load(*m_device, m_root, definition, m_realm, m_textureLenders)) {
        return nullptr;
    }
    m_stocks.push_back(std::move(stock));
    return m_stocks.back().get();
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
std::optional<s32> Critters::spawnGolem(const Vec3& position, f32 yaw) {
    return spawn(Golem::definition(m_realm), position, yaw);
}
std::optional<s32> Critters::spawnGeneral(const Vec3& position, f32 yaw, f32 sight) {
    return spawn(General::definition(), position, yaw, sight);
}
std::optional<s32> Critters::spawnGargoyle(const Vec3& position, f32 yaw, std::string_view form) {
    return spawn(definitionOf(CombatantKind::Gargoyle, form), position, yaw);
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
                                   f32 yaw, f32 sight) {
    auto* stock = stockFor(definition);
    if (stock == nullptr) {
        return std::nullopt;
    }
    for (s32 id = 0; id < kMost; ++id) {
        auto& actor = m_critters[static_cast<usize>(id)];
        if (!actor.present()) {
            if (actor.spawn(*stock, id, position, yaw, m_collision, m_scales, m_realm)) {
                actor.setHazards(m_hazards);
                // A general takes up its round of the lookouts where it is placed, seeing
                // only as far as its placement allows meanwhile (visrad at the level's scale).
                if (definition.patrols && !m_lookouts.empty()) {
                    actor.startPatrol(&m_lookouts, sight * m_scales.sight);
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
        m_pushes.push_back(event);
    }
    for (auto& event : actor.takeGrabs()) {
        m_grabs.push_back(event);
    }
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
    for (const CombatantRam& ram : actor.takeRams()) {
        m_rams.push_back(ram);
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
    for (const auto& stock : m_stocks) {
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
            if (swarm != nullptr) {
                EnemyHit hit;
                hit.damage = trample.damage;
                hit.where = trample.position;
                hit.close = true;
                swarm->hurt(trample.enemy, hit);
            }
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
    return std::exchange(m_blows, {});
}
std::vector<CombatGrab> Critters::takeGrabs() {
    return std::exchange(m_grabs, {});
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
                    const Texture* frozenTexture, const CameraFrame* camera,
                    f32 presentationAlpha) const {
    for (const auto& actor : m_critters) {
        actor.draw(device, clip, lighting, frozenTexture, camera, m_hitFlash, presentationAlpha);
    }
}
} // namespace gdl::game
