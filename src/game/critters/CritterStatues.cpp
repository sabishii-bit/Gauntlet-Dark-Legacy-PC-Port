#include "game/critters/CritterStatues.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/level/NativeStateResources.h"

namespace gdl::game {
namespace {
constexpr s32 kContactGridDimension = 64;
constexpr f32 kItemQueryPadding = 15.0f;

bool sameSpawn(const EnemySpawn& a, const EnemySpawn& b) {
    bool same = false;
    visitNativeFields<16>(a, [&](const auto&... left) {
        visitNativeFields<16>(
            b, [&](const auto&... right) { same = std::tie(left...) == std::tie(right...); });
    });
    return same;
}

template <class T> bool sameField(const T& a, const T& b) {
    return a == b;
}

bool sameField(const std::optional<EnemySpawn>& a, const std::optional<EnemySpawn>& b) {
    return a.has_value() == b.has_value() && (!a || sameSpawn(*a, *b));
}

bool sameField(const std::vector<ItemCollisionTriangle>& a,
               const std::vector<ItemCollisionTriangle>& b) {
    return std::ranges::equal(a, b, [](const auto& first, const auto& second) {
        bool same = false;
        visitNativeFields<2>(first, [&](const auto&... left) {
            visitNativeFields<2>(second, [&](const auto&... right) {
                same = std::tie(left...) == std::tie(right...);
            });
        });
        return same;
    });
}

bool sameField(const ItemInstance& a, const ItemInstance& b) {
    bool same = false;
    visitNativeFields<8>(a, [&](const auto&... left) {
        visitNativeFields<8>(b,
                             [&](const auto&... right) { same = (sameField(left, right) && ...); });
    });
    return same;
}

void appendInstance(NativeStateDigest& digest, const ItemInstance& instance) {
    const auto append = [&]<class T>(const T& field) {
        if constexpr (std::is_same_v<T, std::vector<ItemCollisionTriangle>>) {
            digest.wide(field.size());
            for (const auto& triangle : field) {
                digest.fields<2>(triangle);
            }
        } else {
            digest.value(field);
        }
    };
    visitNativeFields<8>(instance, [&](const auto&... fields) { (append(fields), ...); });
}

bool bounded(f32 value, f32 low = -1'000'000, f32 high = 1'000'000) {
    return std::isfinite(value) && value >= low && value <= high;
}
} // namespace

bool CritterStatues::Placement::operator==(const Placement& other) const {
    bool same = false;
    visitNativeFields<13>(*this, [&](const auto&... left) {
        visitNativeFields<13>(
            other, [&](const auto&... right) { same = (sameField(left, right) && ...); });
    });
    return same;
}

bool CritterStatues::Statue::operator==(const Statue& other) const {
    bool same = false;
    visitNativeFields<7>(*this, [&](const auto&... left) {
        visitNativeFields<7>(
            other, [&](const auto&... right) { same = std::tie(left...) == std::tie(right...); });
    });
    return same;
}

void visitNativeState(NativeStateDigest& digest, const CritterStatues::Placement& value) {
    const auto append = [&]<class T>(const T& field) {
        if constexpr (std::is_same_v<T, ItemInstance>) {
            appendInstance(digest, field);
        } else {
            digest.value(field);
        }
    };
    visitNativeFields<13>(value, [&](const auto&... fields) { (append(fields), ...); });
}

void visitNativeState(NativeStateDigest& digest, const CritterStatues::Statue& value) {
    const auto append = [&]<class T>(const T& field) {
        if constexpr (std::is_same_v<T, ItemArchive*>) {
            digest.resource(field);
        } else {
            digest.value(field);
        }
    };
    visitNativeFields<7>(value, [&](const auto&... fields) { (append(fields), ...); });
}

void visitNativeState(NativeStateDigest& digest, const CritterStatues::Simulation& value) {
    digest.fields<5>(value);
}

bool CritterStatues::archivesLoaded(const Simulation& state) {
    return std::ranges::all_of(state.statues, [](const auto& statue) {
        return !statue || (statue->archive != nullptr && statue->archive->loaded());
    });
}

bool CritterStatues::appendStateDigest(NativeStateDigest& digest, const Simulation& state) {
    if (!archivesLoaded(state)) {
        digest.invalidate();
        return false;
    }
    digest.string("CritterStatues.native.1");
    digest.value(state);
    return digest.good();
}

bool CritterStatues::appendNativeDigest(NativeStateDigest& digest) const {
    return appendStateDigest(digest, m_sim);
}

bool CritterStatues::appendLocalDigest(NativeStateDigest& digest,
                                       const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendStateDigest(digest, saved.m_values);
}

bool CritterStatues::registerNativeResources(NativeStateResources& resources) const {
    if (!archivesLoaded(m_sim)) {
        return false;
    }
    // Compaction removes the live figure, not artwork still borrowed by native
    // history.
    for (const auto& prototype : m_prototypes) {
        if (prototype.archive == nullptr || !prototype.archive->loaded() || !prototype.figure ||
            !prototype.figure->registerNativeResources(resources)) {
            return false;
        }
    }
    for (const auto& statue : m_sim.statues) {
        if (statue && !statue->figure.registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

bool CritterStatues::equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const {
    return canRestoreLocal(a) && canRestoreLocal(b) && a.m_values == b.m_values;
}

void CritterStatues::saveLocal(LocalSnapshot& saved) const {
    saved.m_values = m_sim;
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
}
bool CritterStatues::canRestoreLocal(const LocalSnapshot& saved) const {
    // Same-address asset reload is prohibited while histories borrow the stage;
    // the enclosing stage must invalidate its registry/history before rebinding.
    return saved.m_owner.lock() == m_lifetime && saved.m_epoch == m_localEpoch &&
           archivesLoaded(saved.m_values);
}
bool CritterStatues::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_values;
    return true;
}

std::string_view CritterStatues::treeOf(CombatantKind kind) {
    switch (kind) {
    case CombatantKind::Golem: return kGolemTree;
    case CombatantKind::Gargoyle: return kGargoyleTree;
    default: return {};
    }
}

s32 CritterStatues::activeTicks(s32 frames, s32 rate) {
    const f32 scale = rate > 0 ? static_cast<f32>(rate) * kFramesPerRateUnit : 1.0f;
    const auto whole = static_cast<s32>(std::floor(0.5f + static_cast<f32>(frames) * scale));
    return whole * kTicksPerActiveOn;
}

bool CritterStatues::add(RenderDevice& device, ItemArchive& archive, const Placement& placement,
                         const WorldCollision* collision) {
    std::string_view tree = treeOf(placement.kind);
    if (placement.enemy && placement.enemy->kind == kDeathKind) {
        tree = placement.enemy->tier == 2 ? "DEATHSTATUE2" : "DEATHSTATUE1";
    }
    if (tree.empty() || m_sim.nextInstance == std::numeric_limits<u64>::max()) {
        return false;
    }
    auto statue = std::optional<Statue>{Statue{}};
    statue->placement = placement;
    if (!statue->figure.place(device, archive, tree, placement.instance,
                              placement.onFloor ? collision : nullptr)) {
        return false;
    }
    statue->figure.play(kIdleSequence, true);
    m_collision = collision;
    if (placement.support >= 0 && m_collision != nullptr) {
        if (const auto parent = m_collision->objectTransform(placement.support)) {
            statue->figure.placeAt(*parent * placement.supportLocal);
        }
    }
    statue->instance = m_sim.nextInstance++;
    statue->archive = &archive;
    // Keep immutable art bindings available after the live statue rises.
    m_prototypes.push_back({&archive, std::make_unique<ItemFigure>(statue->figure)});
    m_sim.statues.push_back(std::move(statue));
    ++m_localEpoch;
    return true;
}

void CritterStatues::clear() {
    m_collision = nullptr;
    m_sim.statues.clear();
    m_prototypes.clear();
    m_sim.risen.clear();
    m_sim.contactGridOrigin = Vec2{0};
    m_sim.contactGridInverseWidth = 0;
    ++m_localEpoch;
}

void CritterStatues::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    // Item nodes remain parented even while their supporting trigger disables
    // collision. Carry the dormant body and its ACTIVE animation alike.
    for (auto& statue : m_sim.statues) {
        if (statue->placement.support >= 0) {
            if (const auto parent = m_collision->objectTransform(statue->placement.support)) {
                statue->figure.placeAt(*parent * statue->placement.supportLocal);
            }
        }
    }
}

void CritterStatues::setContactGridBounds(const Vec3& minimum, const Vec3& maximum) {
    m_sim.contactGridOrigin = Vec2{minimum.x, minimum.z};
    const f32 cellWidth =
        std::max(maximum.x - minimum.x, maximum.z - minimum.z) / kContactGridDimension;
    m_sim.contactGridInverseWidth = cellWidth > 0 ? static_cast<f32>(1.0 / cellWidth) : 0;
}

s32 CritterStatues::contactCell(f32 coordinate, f32 origin) const {
    return static_cast<s32>(std::clamp((coordinate - origin) * m_sim.contactGridInverseWidth, 0.0f,
                                       static_cast<f32>(kContactGridDimension - 1)));
}

bool CritterStatues::inContactGrid(const Vec3& statue, const Vec3& player, f32 radius) const {
    // StartEnemyGrid (80043490) visits whole cells, not a second sight circle.
    // InitDynGrid (80043A04) receives a 15-unit item margin in every realm.
    const f32 pad = radius > 0 ? kItemQueryPadding + radius : -radius;
    const s32 x = contactCell(statue.x, m_sim.contactGridOrigin.x);
    const s32 z = contactCell(statue.z, m_sim.contactGridOrigin.y);
    return x >= contactCell(player.x - pad, m_sim.contactGridOrigin.x) &&
           x <= contactCell(player.x + pad, m_sim.contactGridOrigin.x) &&
           z >= contactCell(player.z - pad, m_sim.contactGridOrigin.y) &&
           z <= contactCell(player.z + pad, m_sim.contactGridOrigin.y);
}

void CritterStatues::wake(usize index) {
    if (index < m_sim.statues.size()) {
        m_sim.statues[index]->woken = true;
    }
}

std::optional<std::pair<usize, f32>> CritterStatues::nearestAsleep(const Vec3& spot) const {
    std::optional<std::pair<usize, f32>> nearest;
    for (usize i = 0; i < m_sim.statues.size(); ++i) {
        const Statue& statue = *m_sim.statues[i];
        const f32 distance = glm::distance(statue.figure.position(), spot);
        if (!statue.woken && (!nearest.has_value() || distance < nearest->second)) {
            nearest = std::pair{i, distance};
        }
    }
    return nearest;
}

Obstacle CritterStatues::obstacleOf(const Statue& statue) {
    Obstacle cylinder;
    cylinder.centre = statue.figure.position();
    cylinder.yaw = statue.figure.yaw();
    cylinder.cylinderRadius = statue.placement.radius;
    cylinder.halfAcross = statue.placement.radius;
    cylinder.halfAlong = statue.placement.radius;
    cylinder.height = statue.placement.height;
    return cylinder;
}

bool CritterStatues::wakesAt(const Statue& statue, const Vec3& position, f32 radius, f32 height) {
    const Vec3 away = position - statue.figure.position();
    const f32 reach = statue.placement.sight + radius;
    return away.x * away.x + away.z * away.z <= reach * reach &&
           std::abs(away.y) <= statue.placement.height + 0.5f * height;
}

bool CritterStatues::dormantFor(std::span<const Visitor> visitors) const {
    if (!m_sim.risen.empty()) {
        return false;
    }
    for (const auto& statue : m_sim.statues) {
        if (statue->woken || statue->rising) {
            return false;
        }
        const Obstacle cylinder = obstacleOf(*statue);
        for (const auto& visitor : visitors) {
            if (!bounded(visitor.position.x) || !bounded(visitor.position.y) ||
                !bounded(visitor.position.z) || !bounded(visitor.radius, 0) ||
                !bounded(visitor.height, 0)) {
                return false;
            }
            if (!inContactGrid(statue->figure.position(), visitor.position, visitor.radius)) {
                continue;
            }
            if (statue->placement.sight >= 0) {
                if (wakesAt(*statue, visitor.position, visitor.radius, visitor.height)) {
                    return false;
                }
                // Native contact stops here too when a proximity-waking
                // statue is outside sight, before considering its solid body.
                continue;
            }
            if (cylinder.touchedBy(visitor.position, visitor.radius, 0)) {
                return false;
            }
        }
    }
    return true;
}

Vec3 CritterStatues::touch(const Vec3& position, f32 radius, f32 height) {
    Vec3 stood = position;
    for (auto& statue : m_sim.statues) {
        if (!inContactGrid(statue->figure.position(), position, radius)) {
            continue;
        }
        const Obstacle cylinder = obstacleOf(*statue);
        // The wake probe uses placement sight, independently of the solid body's radius.
        if (statue->placement.sight >= 0.0f) {
            if (!wakesAt(*statue, stood, radius, height)) {
                continue;
            }
            statue->woken = true;
        }
        if (cylinder.touchedBy(stood, radius, 0.0f)) {
            stood = cylinder.pushOut(stood, radius);
        }
    }
    return stood;
}

std::vector<Obstacle> CritterStatues::obstacles() const {
    std::vector<Obstacle> out;
    out.reserve(m_sim.statues.size());
    for (const auto& statue : m_sim.statues) {
        out.push_back(obstacleOf(*statue));
    }
    return out;
}

std::vector<MissileTarget> CritterStatues::targets() const {
    std::vector<MissileTarget> out;
    out.reserve(m_sim.statues.size());
    for (usize i = 0; i < m_sim.statues.size(); ++i) {
        const Statue& statue = *m_sim.statues[i];
        out.push_back(MissileTarget{static_cast<s32>(i), statue.figure.position(),
                                    statue.placement.radius, statue.placement.height});
    }
    return out;
}

void CritterStatues::update(s32 ticks, f32 seconds, const Seen& seen) {
    syncFloors();
    for (auto& statue : m_sim.statues) {
        // Out of view nothing happens to it: neither the waking nor its countdown.
        if (!statue->woken ||
            (seen && !seen(statue->figure.position(), statue->placement.viewRadius))) {
            continue;
        }
        if (!statue->rising) {
            // The ACTIVE sequence starts, and its ticks with it: the record's activeOn, else
            // the sequence's own length (ProcessItems' activetime).
            statue->rising = true;
            statue->figure.play(kActiveSequence, false);
            const TreeSequenceInfo* active = statue->figure.sequenceInfo(kActiveSequence);
            statue->ticksLeft = 0;
            if (statue->placement.activeOn > 0) {
                statue->ticksLeft = statue->placement.activeOn * kTicksPerActiveOn;
            } else if (active != nullptr) {
                statue->ticksLeft = activeTicks(active->frames, active->frameRate);
            }
            continue;
        }
        statue->figure.update(seconds);
        statue->ticksLeft -= ticks;
        if (statue->ticksLeft <= 0) {
            m_sim.risen.push_back(statue->placement);
            statue.reset();
        }
    }
    std::erase_if(m_sim.statues, [](const std::optional<Statue>& statue) { return !statue; });
}

std::vector<CritterStatues::Placement> CritterStatues::takeRisen() {
    auto risen = std::exchange(m_sim.risen, {});
    // Birth consumes the item's current world matrix, not its authored origin.
    // Keep the queued definitions immutable so local rewind retains their node binding.
    for (auto& placement : risen) {
        if (placement.support < 0 || m_collision == nullptr) {
            continue;
        }
        if (const auto parent = m_collision->objectTransform(placement.support)) {
            const Mat4 stood = *parent * placement.supportLocal;
            placement.instance.position = Vec3{stood[3]};
            placement.instance.rotation = {0, -portable::atan2(stood[2].x, stood[2].z), 0};
            if (placement.enemy) {
                placement.enemy->position = placement.instance.position;
                placement.enemy->direction = Vec3{stood[2]};
            }
        }
    }
    return risen;
}

void CritterStatues::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          const CameraFrame* camera) const {
    for (const auto& statue : m_sim.statues) {
        statue->figure.draw(device, clip, lighting, 1.0f, 1.0f, camera);
    }
}
} // namespace gdl::game
