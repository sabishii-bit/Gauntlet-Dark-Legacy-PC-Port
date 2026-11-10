#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"

#include <array>
#include <bit>
#include <set>
#include <string_view>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/formats/LegendsDreamcastCritter.h"
#include "game/bosses/BossDefinition.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/BossKinds.h"

namespace gdl::game {
namespace {
struct Bank {
    s32 kind;
    std::string_view name;
};
// LoadWorld 8c0a3580 selects these original CRITTERS files and MONSTERS banks.
// PBOSS/YETI move metadata exists on disc, but no corresponding shipped arenas/art.
constexpr std::array kBanks{
    Bank{boss_type::kDragon, "dragon"},   Bank{boss_type::kChimera, "chimera"},
    Bank{boss_type::kDjinn, "djinn"},     Bank{boss_type::kDrider, "drider"},
    Bank{boss_type::kSkorne1, "skorne1"}, Bank{boss_type::kSkorne2, "skorne2"}};

/** The shipped bank of a boss kind, or null for a kind no Dreamcast arena stands. */
const Bank* bankOf(s32 kind) {
    for (const auto& bank : kBanks) {
        if (bank.kind == kind) {
            return &bank;
        }
    }
    return nullptr;
}
} // namespace

LegendsDreamcastBossCamera legendsDreamcastBossCamera(std::span<const u8> moves) {
    // Use the same bounded directory/record validator as the combat importer.
    // This is load-time only; no parallel permissive parser accepts bad TYPEs.
    const auto decoded = formats::decodeLegendsDreamcastCritter(moves);
    const usize directory = readU32LE(moves, 0);
    for (usize i = 0; i < readU32LE(moves, 4); ++i) {
        const usize at = directory + i * 12;
        if (readU32LE(moves, at) != 0x54595045U) {
            continue;
        }
        const usize type = readU32LE(moves, at + 4);
        // ProcessCritter 8c08b4e8..500 publishes actor+TYPE+A8 as actor+40.
        // BossRegister 8c0714de supplies actor+8 to CameraAddSubject; its
        // +38 translation therefore reads that attention, not TYPE+AC/B4.
        return {decoded.types.front().floorOffset,
                std::bit_cast<f32>(readU32LE(moves, type + 0xa8)), decoded.types.front().radius};
    }
    throw FormatError("Dreamcast boss camera TYPE is missing");
}

std::unique_ptr<CombatantAssets>
loadLegendsDreamcastBossAssets(RenderDevice& device, const std::filesystem::path& nativeRoot,
                               const std::filesystem::path& commonRoot, s32 kind,
                               std::span<TextureSet* const> textureLenders,
                               std::vector<std::string>& warnings) {
    const auto* bank = bankOf(kind);
    if (bank == nullptr) {
        return {};
    }
    const auto path = AssetLocator(nativeRoot / "critters").find(std::string(bank->name) + ".wad");
    if (!path) {
        throw FileError("Dreamcast boss move data is missing: " + std::string(bank->name));
    }
    auto decoded = formats::decodeLegendsDreamcastCritter(readFile(*path));
    auto archive = loadLegendsDreamcastArchive(nativeRoot / "monsters" / bank->name);
    if (!archive) {
        throw FileError("Dreamcast boss model bank is missing: " + std::string(bank->name));
    }
    const auto family = bossDefinition(bossNameOf(kind));
    // Keep GDL combat/defense mechanics. These records supply native geometry,
    // attack windows, damage, links, sounds and authored effect placement.
    // Capture support is deliberately enabled for imported arenas: a throw must
    // not leave the player falling forever outside the map's finite floor mesh.
    CritterRules rules{.pressureReferenceDamage = decoded.types.front().pressureReferenceDamage,
                       .restoreCaptureSupport = true};
    auto data = CritterData::fromDecoded(family.name, decoded, 0, rules);
    std::vector<CritterData> children;
    for (s32 child = data.childIndex(); child >= 0;) {
        rules.pressureReferenceDamage =
            decoded.types[static_cast<usize>(child)].pressureReferenceDamage;
        auto part =
            CritterData::fromDecoded(family.name, decoded, static_cast<usize>(child), rules);
        child = part.childIndex();
        children.push_back(std::move(part));
    }
    auto result = std::make_unique<CombatantAssets>();
    if (!result->bindDecoded(device, commonRoot, family, 0, std::move(*archive), std::move(data),
                             std::move(children), textureLenders)) {
        throw FormatError("Dreamcast boss body binding failed: " + family.name);
    }
    const auto validateMoves = [&](const CritterData& part) {
        for (const auto& move : part.moves()) {
            if (!result->tree->findSequence(move.anim)) {
                throw FormatError("Dreamcast boss missing native sequence " + part.tree() + ":" +
                                  move.anim);
            }
            if (!move.colnode.empty() &&
                !result->tree->findNode(move.colnode, kCombatantNodeNameLength)) {
                throw FormatError("Dreamcast boss missing native attack node " + move.colnode);
            }
        }
        for (const auto& node : part.parts()) {
            if (!node.node.empty() &&
                !result->tree->findNode(node.node, kCombatantNodeNameLength)) {
                throw FormatError("Dreamcast boss missing native collision node " + node.node);
            }
        }
    };
    validateMoves(result->data);
    for (const auto& child : result->children) {
        validateMoves(child);
    }
    std::set<std::string> missingEffects;
    for (const auto& cue : result->data.sounds()) {
        const bool skin = (cue.flags & effect_flags::kSkin) != 0;
        const bool available = skin ? result->skins.contains(cue.tree)
                                    : result->archive.trees.find(cue.tree).has_value();
        if (cue.shows() && !available && missingEffects.insert(cue.tree).second) {
            warnings.push_back("Dreamcast " + family.name + ": native effect group " + cue.tree +
                               " is absent from its bank; no other platform is substituted");
        }
    }
    // ProcessCritter 8c08ac42 sets a two-second wake deadline; INIT remains the
    // initial pose until native selection requests START, then READY.
    result->wakeDelay = 2;
    result->initialMoveBeforeWake = true;
    return result;
}
} // namespace gdl::game
