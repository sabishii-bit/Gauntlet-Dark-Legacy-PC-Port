#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

struct LegendsN64TowerFigure {
    std::string models;
    std::string animations;
    std::string group;
    std::string sequence;
    Vec3 position{0};
    f32 yaw = 0;
};

struct LegendsN64TowerDoor {
    std::string openingSequence;
    std::string openSequence;
    u32 realmMask = 0;
    u32 realm = 0;
    Vec3 triggerPosition{0};
    f32 triggerRadius = 0;
    u8 requiredObelisks = 0;
};

/** A script-created installed relic. Placement is in the imported map's world
 * units; native drawable flags are not interchangeable with engine flags. */
struct LegendsN64TowerRelic {
    std::string archive;
    std::string model;
    Mat4 placement{1};
    u32 nativeDrawFlags = 0;
};

/** Script-created collision, already converted from native 1/32-unit fields.
 * Both shapes use a centre and half extents; cylinder X/Z are its radius.
 * Yaw follows the common engine's +Y rotation, not the native negative yaw. */
struct LegendsN64TowerVolume {
    enum class Shape { Cylinder, Box };
    Shape shape = Shape::Cylinder;
    Vec3 centre{0};
    Vec3 halfExtents{0};
    f32 yaw = 0;
    u16 nativeCategory = 0;
};

/** Figures created by the Tower overlay, not by IPR placements. Door sequence
 * transforms are authored in items/tower; no placement offset is applied. */
struct LegendsN64Tower {
    LegendsN64TowerFigure sumner;
    LegendsN64TowerFigure podium;
    std::array<LegendsN64TowerDoor, 4> doors;
    std::array<LegendsN64TowerVolume, 2> residentVolumes;
    std::string staffModel;
    std::string staffParent;
    bool staffVisibleInIdle = false;
    std::vector<std::string> initiallyHiddenWorldObjects;
    std::array<LegendsN64TowerRelic, 13> runestones;
    std::array<LegendsN64TowerRelic, 4> shards;
    LegendsN64TowerFigure shardPortal;
    f32 shardPortalScale = 1;
};

/** Reads constants from the supported native US Tower overlay. A different
 * executable is rejected rather than treating its offsets as this revision. */
LegendsN64Tower decodeLegendsN64Tower(std::span<const u8> overlay);

} // namespace gdl::formats
