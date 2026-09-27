#include "game/world/Rotators.h"

#include <cmath>
#include <cstring>

#include "engine/core/Types.h"

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

void Rotators::bind(const WorldLayout& layout) {
    constexpr usize kTargetAt = 0;
    constexpr usize kSpeedAt = 4;
    constexpr usize kLimitAt = 8;
    m_rotators.clear();
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
        m_rotators.push_back(std::move(rotator));
    }
}

void Rotators::bindFigures(RenderDevice& device, const WorldLayout& layout, ItemArchive& items) {
    for (Rotator& rotator : m_rotators) {
        rotator.pad.reset();
        if (rotator.subtype != kTurnedByPad) {
            continue;
        }
        const ItemInstance& instance = layout.itemInstances()[static_cast<usize>(rotator.instance)];
        const ItemInfo& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        auto pad = std::make_unique<ItemFigure>();
        if (pad->place(device, items, info.name, instance, nullptr)) {
            rotator.pad = std::move(pad);
        }
    }
}

std::vector<RotatorCue> Rotators::update(f32 seconds, std::span<const TriggerVisitor> visitors,
                                         WorldScene& scene) {
    std::vector<RotatorCue> cues;
    const f32 ticks = seconds * kTicksPerSecond;
    for (usize i = 0; i < m_rotators.size(); ++i) {
        Rotator& rotator = m_rotators[i];
        const Vec3 at = scene.worldTransform(static_cast<usize>(rotator.object))[3];
        if (rotator.subtype == kSpinning) {
            rotator.turned = std::remainder(rotator.turned + rotator.speed * ticks, 2.0f * kPi);
            place(rotator, scene);
            continue;
        }
        if (rotator.done) {
            continue;
        }
        if (!rotator.started) {
            for (const TriggerVisitor& visitor : visitors) {
                const Vec3 away = visitor.position - rotator.spot;
                const f32 reach = rotator.radius + visitor.radius;
                if (away.x * away.x + away.z * away.z <= reach * reach &&
                    std::abs(away.y) <= kReach) {
                    rotator.started = true;
                }
            }
            if (!rotator.started) {
                continue;
            }
        }
        // It turns until it has gone its angle, then stops where it is (the last step may
        // carry it a little past, as the original's does).
        const bool reached = rotator.speed >= 0.0f ? rotator.turned >= rotator.limit
                                                   : rotator.turned <= -rotator.limit;
        if (reached) {
            rotator.done = true;
            cues.push_back(RotatorCue{RotatorCue::Kind::Stopped, i, at});
            continue;
        }
        rotator.turned += rotator.speed * ticks;
        place(rotator, scene);
        cues.push_back(RotatorCue{RotatorCue::Kind::Turning, i, at});
    }
    return cues;
}

void Rotators::place(const Rotator& rotator, WorldScene& scene) {
    scene.setObjectTransform(static_cast<usize>(rotator.object),
                             glm::rotate(glm::translate(Mat4{1.0f}, rotator.origin), rotator.turned,
                                         Vec3{0.0f, 1.0f, 0.0f}));
}

void Rotators::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const Rotator& rotator : m_rotators) {
        if (rotator.pad != nullptr) {
            rotator.pad->draw(device, clip, lighting);
        }
    }
}

} // namespace gdl::game
