#include "expansions/dreamcast/level/LegendsDreamcastFalling.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"

namespace gdl::game {
namespace {
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

void LegendsDreamcastFalling::clear(f32 worldMinY) {
    m_pieces.clear();
    m_remainder = 0;
    // LoadWorld 8c0a28cc sets the floor sentinel to bounds.minY - 4.5;
    // DoItems removes falling rubble two hundred units below that sentinel.
    m_bottom = worldMinY - FallingPiece::kFloorSentinelMargin - FallingPiece::kDiscardDepth;
}

bool LegendsDreamcastFalling::add(const formats::ItemInfoRecord& info,
                                  const formats::ItemInstanceRecord& item, u32 nativeIndex,
                                  usize figure) {
    const s16 override = std::bit_cast<s16>(readU16LE(item.params, 0));
    const s32 subtype = override > 0 ? override : info.subtype;
    if (info.type != 10 || (subtype != 40 && subtype != 53) || info.collisionType != 1 ||
        !finite(item.position) || !finite(item.rotation) || !finite(info.collisionOffset) ||
        !std::isfinite(info.radius) || info.radius <= 0 || !std::isfinite(info.height) ||
        info.height <= 0 || std::ranges::any_of(m_pieces, [&](const Piece& piece) {
            return piece.nativeIndex == nativeIndex || piece.figure == figure;
        })) {
        return false;
    }
    Piece piece;
    piece.figure = figure;
    piece.nativeIndex = nativeIndex;
    piece.subtype = subtype;
    piece.body.place(item.position, item.rotation, nativeIndex);
    piece.active = (info.activeType & 1) != 0;
    // SetItem copies the authored matrix and initializes velocity to zero.
    // AddItemSub leaves airborne flag 1 rubble alone; never put it at a
    // neighboring platform's final key or snap it beneath the lava.
    const Vec3 offset = info.collisionOffset + Vec3{0, 1, 0};
    piece.contact.centre =
        Vec3{legendsDreamcastItemPlacement(item.position, item.rotation) * Vec4{offset, 1}};
    piece.contact.cylinderRadius = info.radius;
    piece.contact.height = info.height;
    piece.contact.solid = false; // ItemTouch 40/53 activates without a blocking response.
    m_pieces.push_back(piece);
    return true;
}

std::vector<Vec3> LegendsDreamcastFalling::touch(std::span<const LegendsPortalVisitor> visitors) {
    std::vector<Vec3> activated;
    for (auto& piece : m_pieces) {
        if (piece.active || !piece.body.visible) {
            continue;
        }
        for (const auto& visitor : visitors) {
            if (!visitor.alive || visitor.player >= 4 || !finite(visitor.position) ||
                !std::isfinite(visitor.radius) || visitor.radius < 0 ||
                !std::isfinite(visitor.height) || visitor.height <= 0) {
                continue;
            }
            const Vec3 centre = visitor.position + Vec3{0, visitor.height * .5f, 0};
            if (std::abs(centre.y - piece.contact.centre.y) >
                piece.contact.height + visitor.height * .5f) {
                continue;
            }
            if (!piece.contact.touchedBy({centre.x, piece.contact.centre.y, centre.z},
                                         visitor.radius, 0)) {
                continue;
            }
            // ItemTouch 8c0af0e0, case 10/subtypes 40/53: one collapse cue and
            // active bit 1. It neither damages nor pushes the touching player.
            piece.active = true;
            activated.push_back(piece.body.position);
            break;
        }
    }
    return activated;
}

void LegendsDreamcastFalling::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 10) {
        return;
    }
    const s32 frames = FallingPiece::framesDue(m_remainder, seconds);
    for (auto& piece : m_pieces) {
        if (!piece.active) {
            continue;
        }
        // DoItems 8c0ad154..2ac uses the same {-4,-3,-2,-1,1,2,3,4}
        // table (8c123a1c) and gravity 2 as FallingPiece. Subtype 53 selects
        // one-degree spin at 8c0ad1be, rather than the falling rock's twenty.
        // The native Stop Time pass only changes trap actions, not rubble.
        for (s32 frame = 0; frame < frames && piece.body.visible; ++frame) {
            piece.body.advance(FallingProfile::of(piece.subtype), m_bottom);
        }
    }
}

bool LegendsDreamcastFalling::visible(usize figure) const {
    const auto piece = std::ranges::find(m_pieces, figure, &Piece::figure);
    return piece == m_pieces.end() || piece->body.visible;
}
} // namespace gdl::game
