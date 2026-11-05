#include "game/level/Rotators.h"

#include <cmath>
#include <cstring>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/level/NativeStateResources.h"

namespace gdl::game {

namespace {

s32 paramS32(const ItemInstance& instance, usize at) {
    s32 value = 0;
    if (at + sizeof(value) <= instance.params.size()) {
        std::memcpy(&value, &instance.params[at], sizeof(value));
    }
    return value;
}

f32 paramF32(const ItemInstance& instance, usize at) {
    f32 value = 0.0f;
    if (at + sizeof(value) <= instance.params.size()) {
        std::memcpy(&value, &instance.params[at], sizeof(value));
    }
    return value;
}

} // namespace

bool Rotators::appendDigest(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("Rotators/1");
    digest.resource(m_layout);
    digest.value(state);
    return digest.good();
}

bool Rotators::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, m_sim);
}

bool Rotators::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [state, binding, owner, epoch] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, saved.m_state);
}

bool Rotators::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& rotator : m_sim.rotators) {
        if (rotator.pad && !rotator.pad->registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

void Rotators::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
    saved.m_epoch = m_localEpoch;
}

bool Rotators::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding && saved.m_epoch == m_localEpoch;
}

bool Rotators::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state;
}

bool Rotators::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

void Rotators::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_layout = nullptr;
    m_sim = {};
    m_localEpoch = 0;
}

void Rotators::bind(const WorldLayout& layout) {
    constexpr usize kTargetAt = 0;
    constexpr usize kSpeedAt = 4;
    constexpr usize kLimitAt = 8;
    clear();
    m_layout = &layout;
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<WorldObject>& objects = layout.objects();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kRotator ||
            (info.subtype != kSpinning && info.subtype != kTurnedByPad)) {
            continue;
        }
        const s32 object = paramS32(instance, kTargetAt);
        if (object < 0 || static_cast<usize>(object) >= objects.size()) {
            continue;
        }
        Rotator rotator;
        rotator.instance = static_cast<s32>(i);
        rotator.object = object;
        rotator.subtype = info.subtype;
        rotator.speed = paramF32(instance, kSpeedAt);
        rotator.limit = paramF32(instance, kLimitAt);
        rotator.origin = objects[static_cast<usize>(object)].position;
        rotator.spot = instance.position;
        rotator.radius = info.radius;
        rotator.height = info.height;
        m_sim.rotators.push_back(std::move(rotator));
    }
}

void Rotators::bindFigures(RenderDevice& device, const WorldLayout& layout,
                           const AssetScope& scope) {
    m_checkpointBinding = std::make_shared<const u8>(0);
    for (Rotator& rotator : m_sim.rotators) {
        rotator.pad.reset();
        if (rotator.subtype != kTurnedByPad) {
            continue;
        }
        const ItemInstance& instance = layout.itemInstances()[static_cast<usize>(rotator.instance)];
        if ((instance.flags & ItemInstance::kNoGeometry) != 0) {
            continue;
        }
        const ItemInfo& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        ItemFigure pad;
        if (pad.place(device, scope, info.name, instance, nullptr)) {
            rotator.pad = std::move(pad);
        }
    }
}

std::vector<RotatorCue> Rotators::update(f32 seconds, std::span<const TriggerVisitor> visitors,
                                         WorldScene& scene) {
    std::vector<RotatorCue> cues;
    const f32 ticks = seconds * kTicksPerSecond;
    for (usize i = 0; i < m_sim.rotators.size(); ++i) {
        Rotator& rotator = m_sim.rotators[i];
        const Vec3 at = scene.worldTransform(static_cast<usize>(rotator.object))[3];
        if (rotator.subtype != kSpinning && !rotator.done && !rotator.started) {
            for (const TriggerVisitor& visitor : visitors) {
                const Vec3 away = visitor.position - rotator.spot;
                const f32 reach = rotator.radius + visitor.radius;
                if (away.x * away.x + away.z * away.z <= reach * reach &&
                    std::abs(away.y) <= rotator.height + visitor.height * 0.5f) {
                    rotator.started = true;
                }
            }
        }
        if (const auto cue = advance(rotator, ticks, scene)) {
            cues.push_back(RotatorCue{*cue, i, at});
        }
    }
    return cues;
}

std::optional<RotatorCue::Kind> Rotators::advance(Rotator& rotator, f32 ticks, WorldScene& scene) {
    if (rotator.subtype == kSpinning) {
        rotator.turned = std::remainder(rotator.turned + rotator.speed * ticks, 2.0f * kPi);
        place(rotator, scene);
        return std::nullopt;
    }
    if (rotator.done || !rotator.started) {
        return std::nullopt;
    }
    // It turns until it has gone its angle, then stops where it is (the last step may
    // carry it a little past, as the original's does).
    const bool reached =
        rotator.speed >= 0.0f ? rotator.turned >= rotator.limit : rotator.turned <= -rotator.limit;
    if (reached) {
        rotator.done = true;
        return RotatorCue::Kind::Stopped;
    }
    rotator.turned += rotator.speed * ticks;
    place(rotator, scene);
    return RotatorCue::Kind::Turning;
}

bool Rotators::Cursor::valid() const {
    return instance >= 0 && instance < 65535 && object >= 0 && object < 65535 &&
           std::isfinite(turned) && std::abs(turned) <= 1'000'000 && (!done || started);
}

std::vector<Rotators::Cursor> Rotators::checkpoint() const {
    std::vector<Cursor> result;
    result.reserve(m_sim.rotators.size());
    for (const auto& rotator : m_sim.rotators) {
        result.push_back(
            {rotator.instance, rotator.object, rotator.turned, rotator.started, rotator.done});
    }
    return result;
}

bool Rotators::accepts(std::span<const Cursor> state) const {
    if (state.size() != m_sim.rotators.size() || state.size() > kMaxCursors) {
        return false;
    }
    for (usize index = 0; index < state.size(); ++index) {
        const auto& cursor = state[index];
        const auto& rotator = m_sim.rotators[index];
        if (!cursor.valid() || cursor.object != rotator.object ||
            cursor.instance != rotator.instance || !std::isfinite(rotator.speed) ||
            !std::isfinite(rotator.limit) ||
            (rotator.subtype == kSpinning &&
             (cursor.started || cursor.done || std::abs(cursor.turned) > kPi))) {
            return false;
        }
    }
    return true;
}

bool Rotators::restore(std::span<const Cursor> state) {
    if (!accepts(state)) {
        return false;
    }
    ++m_localEpoch;
    for (usize index = 0; index < state.size(); ++index) {
        auto& rotator = m_sim.rotators[index];
        const auto& cursor = state[index];
        rotator.turned = cursor.turned;
        rotator.started = cursor.started;
        rotator.done = cursor.done;
    }
    return true;
}

bool Rotators::advanceMotion(f32 seconds, WorldScene& scene) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 0.25f) {
        return false;
    }
    for (auto& rotator : m_sim.rotators) {
        advance(rotator, seconds * kTicksPerSecond, scene);
    }
    return true;
}

void Rotators::place(const Rotator& rotator, WorldScene& scene) {
    const auto object = static_cast<usize>(rotator.object);
    // YawMat3 (800be4f4) leaves translation intact. Ice Camp's gears also have
    // height switches; rotating must not put them back at their authored height.
    const Vec3 position = scene.localTransform(object)[3];
    scene.setObjectTransform(object, portable::rotate(glm::translate(Mat4{1.0f}, position),
                                                      rotator.turned, Vec3{0.0f, 1.0f, 0.0f}));
}

void Rotators::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const Rotator& rotator : m_sim.rotators) {
        if (rotator.pad) {
            rotator.pad->draw(device, clip, lighting);
        }
    }
}

} // namespace gdl::game
