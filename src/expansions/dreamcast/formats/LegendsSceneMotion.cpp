#include "expansions/dreamcast/formats/LegendsSceneMotion.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void validate(const NodeTrack& track, s32 frames) {
    if (frames < 0 || track.frames.empty() || track.frames.front() != 0 ||
        (frames == 0 && track.frames.size() != 1) ||
        (frames > 0 && track.frames.back() >= frames) ||
        track.values.size() != track.frames.size() * track.channelCount() ||
        !std::is_sorted(track.frames.begin(), track.frames.end()) ||
        std::adjacent_find(track.frames.begin(), track.frames.end()) != track.frames.end() ||
        !std::all_of(track.values.begin(), track.values.end(),
                     [](f32 value) { return std::isfinite(value); })) {
        throw FormatError("Dreamcast animation has invalid keyframe values");
    }
}

std::vector<s32> parentsOf(const std::vector<WorldObjectRecord>& objects) {
    std::vector<s32> parents(objects.size(), -1);
    for (usize parent = 0; parent < objects.size(); ++parent) {
        const auto& object = objects[parent];
        if (!finite(object.position) || !std::isfinite(object.radius) || object.nextIndex < -1 ||
            object.childIndex < -1 || object.nextIndex >= static_cast<s32>(objects.size()) ||
            object.childIndex >= static_cast<s32>(objects.size())) {
            throw FormatError("Dreamcast world has an invalid hierarchy node");
        }
        s32 child = object.childIndex;
        usize visited = 0;
        while (child >= 0) {
            if (++visited > objects.size() || child == static_cast<s32>(parent) ||
                parents[static_cast<usize>(child)] != -1) {
                throw FormatError("Dreamcast world has duplicate or cyclic child links");
            }
            parents[static_cast<usize>(child)] = static_cast<s32>(parent);
            child = objects[static_cast<usize>(child)].nextIndex;
            if (child < -1 || child >= static_cast<s32>(objects.size())) {
                throw FormatError("Dreamcast world sibling lies outside the object table");
            }
        }
    }
    // Root siblings are not visited by a parent's child list, but must still be
    // acyclic: a native traversal may start at any root in this object table.
    std::vector<u8> siblings(objects.size(), 0);
    for (usize begin = 0; begin < objects.size(); ++begin) {
        s32 node = static_cast<s32>(begin);
        while (node >= 0 && siblings[static_cast<usize>(node)] == 0) {
            siblings[static_cast<usize>(node)] = 1;
            node = objects[static_cast<usize>(node)].nextIndex;
        }
        if (node >= 0 && siblings[static_cast<usize>(node)] == 1) {
            throw FormatError("Dreamcast world has cyclic root siblings");
        }
        node = static_cast<s32>(begin);
        while (node >= 0 && siblings[static_cast<usize>(node)] == 1) {
            siblings[static_cast<usize>(node)] = 2;
            node = objects[static_cast<usize>(node)].nextIndex;
        }
    }
    // A longer ancestor cycle need not repeat any individual child list.
    std::vector<u8> visited(objects.size(), 0);
    for (usize begin = 0; begin < objects.size(); ++begin) {
        s32 node = static_cast<s32>(begin);
        while (node >= 0 && visited[static_cast<usize>(node)] == 0) {
            visited[static_cast<usize>(node)] = 1;
            node = parents[static_cast<usize>(node)];
        }
        if (node >= 0 && visited[static_cast<usize>(node)] == 1) {
            throw FormatError("Dreamcast world has cyclic ancestors");
        }
        node = static_cast<s32>(begin);
        while (node >= 0 && visited[static_cast<usize>(node)] == 1) {
            visited[static_cast<usize>(node)] = 2;
            node = parents[static_cast<usize>(node)];
        }
    }
    return parents;
}

} // namespace

AnimationFile parseDreamcastAnimation(std::span<const u8> bytes) {
    if (bytes.size() < 16 || readU16LE(bytes, 2) != 0) {
        throw FormatError("unsupported Dreamcast animation archive revision");
    }
    auto result = AnimationFile::parse(bytes);
    for (const auto& tree : result.trees) {
        for (const auto& node : tree.nodes) {
            if (node.parent < -1 || !finite(node.position) ||
                static_cast<u32>(node.type) > static_cast<u32>(TreeNodeType::Particles)) {
                throw FormatError("Dreamcast animation has an invalid tree node");
            }
        }
        for (const auto& sequence : tree.sequences) {
            if (sequence.frameCount < 0 || sequence.frameRate < 0) {
                throw FormatError("Dreamcast animation has an invalid sequence clock");
            }
            for (const auto& track : sequence.tracks) {
                if (track.node >= tree.nodes.size()) {
                    throw FormatError("Dreamcast animation track has no node");
                }
                validate(track, sequence.frameCount);
            }
        }
    }
    return result;
}

LegendsSceneMotion LegendsSceneMotion::parseDreamcast(std::span<const u8> worlds,
                                                      std::span<const u8> animations) {
    // All canonical retail maps have the version-2 world extension. Older backup
    // directories on disc use version 1 and must not be mistaken for that layout.
    if (worlds.size() < 120 || readU32LE(worlds, 96) != 0xf00bab02U) {
        throw FormatError("unsupported Dreamcast world motion revision");
    }
    auto world = WorldFile::parse(worlds);
    LegendsSceneMotion result;
    result.objects = std::move(world.objects);
    result.parents = parentsOf(result.objects);
    result.worldAnimations = std::move(world.animations);
    result.worldParticles = std::move(world.particles);
    for (const auto& animation : result.worldAnimations) {
        if (animation.objectIndex < 0 ||
            animation.objectIndex >= static_cast<s32>(result.objects.size()) ||
            !std::isfinite(animation.startFrame)) {
            throw FormatError("Dreamcast world animation has an invalid target or start frame");
        }
        validate(animation.track, animation.frameCount);
    }
    if (!animations.empty()) {
        result.archive = parseDreamcastAnimation(animations);
    }
    usize unresolved = 0;
    for (const auto& tree : result.archive.trees) {
        for (const auto& node : tree.nodes) {
            unresolved += node.type == TreeNodeType::Particles && node.particle < 0 ? 1U : 0U;
        }
    }
    if (unresolved != 0) {
        result.warnings.push_back(std::format("{} native particle nodes have no embedded template; "
                                              "no substitute effect is generated.",
                                              unresolved));
    }
    return result;
}

} // namespace gdl::formats
