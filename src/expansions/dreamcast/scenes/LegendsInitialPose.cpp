#include "expansions/dreamcast/scenes/LegendsInitialPose.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/Error.h"
#include "engine/world/TreePose.h"

#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"

namespace gdl::game {
namespace {
constexpr u32 kDynamic = 0x01001000U;
constexpr u32 kDisabled = 0x00300000U;

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const Mat4& value) {
    return std::ranges::all_of(std::span<const f32, 16>{glm::value_ptr(value), 16},
                               [](f32 component) { return std::isfinite(component); });
}

} // namespace

void applyDreamcastInitialPose(formats::LegendsMap& map,
                               const formats::LegendsSceneMotion& motion) {
    const usize count = motion.objects.size();
    if (map.objects.size() != count || motion.parents.size() != count) {
        throw FormatError("Dreamcast initial pose has a different object roster");
    }
    std::vector<NodePose> poses(count);
    for (const auto& animation : motion.worldAnimations) {
        if (animation.objectIndex < 0 || static_cast<usize>(animation.objectIndex) >= count ||
            !std::isfinite(animation.startFrame) || animation.frameCount < 0 ||
            animation.track.frames.empty() ||
            animation.track.values.size() !=
                animation.track.frames.size() * animation.track.channelCount()) {
            throw FormatError("Dreamcast initial pose has an invalid animation");
        }
        const auto object = static_cast<usize>(animation.objectIndex);
        if ((motion.objects[object].flags & kDisabled) == kDisabled) {
            continue;
        }
        // DoWorldAnimSub (8c0e9700) samples curframe before advancing. WorldLoad
        // resets state, not curframe: a reversed track still starts at its authored
        // frame, rather than unconditionally being moved to its final key.
        const TrackInfo track{animation.track.node, animation.track.flags, animation.track.frames,
                              animation.track.values};
        const NodePose sample = TreePose::sample(track, animation.startFrame);
        auto& pose = poses[object];
        if ((track.flags & 0x7U) != 0) {
            pose.rotation = sample.rotation;
            pose.rotationOrder = sample.rotationOrder;
        }
        if ((track.flags & 0x70U) != 0) {
            pose.position = sample.position;
        }
        if ((track.flags & 0x700U) != 0) {
            pose.scale = sample.scale;
        }
    }

    std::vector<Mat4> matrices(count);
    std::vector<u8> ready(count, 0);
    std::vector<usize> path;
    for (usize begin = 0; begin < count; ++begin) {
        path.clear();
        s32 current = static_cast<s32>(begin);
        while (current >= 0) {
            if (static_cast<usize>(current) >= count) {
                throw FormatError("Dreamcast initial pose has an invalid parent");
            }
            const auto node = static_cast<usize>(current);
            if (ready[node] == 2) {
                break;
            }
            if (ready[node] == 1) {
                throw FormatError("Dreamcast initial pose has cyclic parents");
            }
            ready[node] = 1;
            path.push_back(node);
            current = motion.parents[node];
            if (current < -1) {
                throw FormatError("Dreamcast initial pose has an invalid parent");
            }
        }
        while (!path.empty()) {
            const usize node = path.back();
            path.pop_back();
            const s32 parent = motion.parents[node];
            matrices[node] = TreePose::localMatrix(poses[node], motion.objects[node].position);
            if (parent >= 0) {
                matrices[node] = matrices[static_cast<usize>(parent)] * matrices[node];
            }
            if (!finite(matrices[node]) || !finite(map.objects[node].transform)) {
                throw FormatError("Dreamcast initial pose has a nonfinite transform");
            }
            ready[node] = 2;
        }
    }

    std::vector<Mat4> changes(count, Mat4{1});
    std::vector<bool> changed(count, false);
    for (usize i = 0; i < count; ++i) {
        if (matrices[i] == map.objects[i].transform) {
            continue;
        }
        if (glm::determinant(map.objects[i].transform) == 0.0f) {
            throw FormatError("Dreamcast initial pose cannot replace a singular prior transform");
        }
        changes[i] = matrices[i] * glm::inverse(map.objects[i].transform);
        if (!finite(changes[i])) {
            throw FormatError("Dreamcast initial pose has an invalid collision transform");
        }
        changed[i] = true;
    }

    // WorldObjCollide (8c0ea060) transforms only dynamic owner records. The
    // decoder has already translated those local planes into the initial world
    // space; static triangles must never receive their owner's transform twice.
    auto collision = map.collision;
    for (auto& triangle : collision) {
        if (triangle.owner < 0 || static_cast<usize>(triangle.owner) >= count) {
            throw FormatError("Dreamcast initial pose collision has no native owner");
        }
        const auto owner = static_cast<usize>(triangle.owner);
        const auto& record = motion.objects[owner];
        if ((record.flags & kDynamic) == 0 || !changed[owner]) {
            continue;
        }
        const Mat4& change = changes[owner];
        for (auto& vertex : triangle.vertices) {
            vertex = Vec3(change * Vec4(vertex, 1.0f));
            if (!finite(vertex)) {
                throw FormatError("Dreamcast initial pose has nonfinite collision vertices");
            }
        }
        const Mat3 linear(change);
        const f32 determinant = glm::determinant(linear);
        if (determinant == 0.0f) {
            // A collapsed authored object has no traversable surface. Keep a
            // finite normal for inspection instead of inverting a singular scale.
            triangle.floor = false;
            triangle.wall = false;
            continue;
        }
        const Vec3 normal = glm::transpose(glm::inverse(linear)) * triangle.normal;
        const f32 length = glm::length(normal);
        if (!finite(normal) || !std::isfinite(length) || length == 0.0f) {
            throw FormatError("Dreamcast initial pose has an invalid collision normal");
        }
        triangle.normal = normal / length;
        const s32 parent = motion.parents[owner];
        const bool enabled =
            !record.noCollision &&
            (parent < 0 || !motion.objects[static_cast<usize>(parent)].noCollision);
        triangle.floor = enabled && (record.flags & 0x23cU) != 0 && triangle.normal.y >= 0.5f;
        triangle.wall = enabled && (record.flags & 0x13aU) != 0;
    }
    for (usize i = 0; i < count; ++i) {
        map.objects[i].transform = matrices[i];
    }
    map.collision = std::move(collision);
}

} // namespace gdl::game
