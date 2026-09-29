#include "game/world/FallingScenery.h"

#include <array>
#include <cmath>
#include <string_view>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
constexpr usize kRealms = 11; ///< the realms lettered A to K
/** fn_8009D91C's table (sounds_evt.c lbl_80123AAC) by realm, A first. */
constexpr std::array<std::string_view, kRealms> kBreakSounds{"S_FALLAWAY",
                                                             "S_ROCKBREAK",
                                                             "S_LIMBBREAKC",
                                                             "S_LIMBBREAK",
                                                             "S_ROCKBREAKE",
                                                             "S_ROCKBREAKF",
                                                             "S_ROCKBREAKG",
                                                             "S_LIMBBREAKH",
                                                             "S_ICEBREAK",
                                                             "",
                                                             ""};
/** fn_8009D8CC's table (lbl_80123AE4). */
constexpr std::array<std::string_view, kRealms> kLeafSounds{
    "", "", "", "S_LEAFBREAK", "", "", "", "", "S_WOODBREAKI", "", ""};
constexpr std::string_view kForestFirstLevelSound = "S_ROCKBREAKF2"; ///< F1's own
constexpr std::string_view kIceFourthLevelSound = "S_ICEBREAKY";     ///< I4's own
constexpr std::string_view kForestFirstLevel = "F1";
constexpr std::string_view kIceFourthLevel = "I4";

std::string_view soundOf(const std::array<std::string_view, kRealms>& table,
                         std::string_view levelName) {
    if (levelName.empty() || levelName.front() < 'A' || levelName.front() > 'K') {
        return {};
    }
    return table[static_cast<usize>(levelName.front() - 'A')];
}
} // namespace

std::string_view FallingScenery::breakSoundOf(std::string_view levelName) {
    if (levelName == kForestFirstLevel) {
        return kForestFirstLevelSound;
    }
    if (levelName == kIceFourthLevel) {
        return kIceFourthLevelSound;
    }
    return soundOf(kBreakSounds, levelName);
}

std::string_view FallingScenery::leafSoundOf(std::string_view levelName) {
    return soundOf(kLeafSounds, levelName);
}

void FallingScenery::clear() {
    m_pieces.clear();
    m_breakSound = {};
    m_leafSound = {};
    m_remainder = 0.0f;
    m_bottom = 0.0f;
}

void FallingScenery::bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
                          TextureSet& textures, std::string_view levelName) {
    clear();
    m_breakSound = breakSoundOf(levelName);
    m_leafSound = leafSoundOf(levelName);
    m_bottom = FallingPiece::bottomOf(layout);
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const auto& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const auto& info = infos[static_cast<usize>(instance.info)];
        const auto given =
            static_cast<s32>(instance.params[0]) | (static_cast<s32>(instance.params[1]) << 8);
        const s32 subtype = given > 0 ? given : info.subtype;
        if (info.type != kObstacle || (subtype != kFallAway && subtype != kLeafFall &&
                                       subtype != kShootFall && subtype != kRockSink)) {
            continue;
        }
        TreeInfo tree;
        tree.name = instance.name;
        TreeNodeInfo node;
        node.name = instance.name;
        node.object = instance.name;
        node.objectFlags = info.objectFlags;
        tree.nodes.push_back(node);
        Piece piece;
        if (!piece.model.bind(tree, models, textures, device)) {
            log::warn("Falling scenery mesh missing: {}", instance.name);
            continue;
        }
        piece.motion.place(instance.position, instance.rotation, i);
        piece.profile = FallingProfile::of(subtype);
        piece.subtype = subtype;
        piece.minPlayers = instance.minPlayers;
        piece.radius = info.radius;
        piece.height = info.height;
        piece.centre = info.collisionOffset;
        m_pieces.push_back(std::move(piece));
    }
    setPlayerCount(1);
}

void FallingScenery::setPlayerCount(s32 players) {
    for (Piece& piece : m_pieces) {
        piece.shown = shownToParty(piece.minPlayers, players);
    }
}

bool FallingScenery::within(const Piece& piece, const Vec3& position, f32 radius) {
    // fn_8005F0F4's cylinder: the record's radius and height, each grown by the body's.
    const Vec3 centre = piece.motion.position + piece.centre;
    const f32 reach = piece.radius + radius;
    const f32 dx = position.x - centre.x;
    const f32 dz = position.z - centre.z;
    return dx * dx + dz * dz <= reach * reach &&
           std::abs(position.y - centre.y) <= piece.height + radius;
}

std::vector<FallingCue> FallingScenery::start(const Vec3& position, f32 radius, bool shot) {
    std::vector<FallingCue> cues;
    for (Piece& piece : m_pieces) {
        if (piece.started || !piece.shown || (piece.subtype == kShootFall) != shot ||
            !within(piece, position, radius)) {
            continue;
        }
        piece.started = true;
        cues.push_back(FallingCue{piece.motion.position,
                                  piece.subtype == kLeafFall ? m_leafSound : m_breakSound});
    }
    return cues;
}

std::vector<FallingCue> FallingScenery::touch(const Vec3& position, f32 radius) {
    return start(position, radius, false);
}

std::vector<FallingCue> FallingScenery::shoot(const Vec3& position, f32 radius) {
    return start(position, radius, true);
}

void FallingScenery::update(f32 seconds) {
    for (s32 frame = FallingPiece::framesDue(m_remainder, seconds); frame > 0; --frame) {
        for (Piece& piece : m_pieces) {
            if (piece.started) {
                piece.motion.advance(piece.profile, m_bottom);
            }
        }
    }
}

void FallingScenery::draw(RenderDevice& device, const Mat4& clip,
                          const WorldLighting& lighting) const {
    for (const Piece& piece : m_pieces) {
        if (piece.shown && piece.motion.visible) {
            piece.model.draw(device, clip,
                             itemPlacement(piece.motion.position, piece.motion.rotation), lighting);
        }
    }
}
} // namespace gdl::game
