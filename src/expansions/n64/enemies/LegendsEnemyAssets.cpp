#include "expansions/n64/enemies/LegendsEnemyAssets.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string_view>

#include "engine/core/Error.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/ByteReader.h"

#include "expansions/n64/assets/LegendsN64Archive.h"
#include "expansions/n64/formats/LegendsN64SequenceGroups.h"
#include "formats/AnimationTree.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {
struct Bank {
    s32 kind;
    std::string_view directory;
    std::string_view prefix;
};
// e006c0c4 supplies archive names; enemy.rom supplies the corresponding
// group prefixes. Common species IDs are deliberately not native indices.
constexpr std::array kBanks{Bank{enemy_type::kScorpion, "scoh", "SCO"},
                            Bank{enemy_type::kTroll, "tro", "TRO"},
                            Bank{enemy_type::kDemon, "dem", "DEM"},
                            Bank{enemy_type::kRat, "rat", "RAT"},
                            Bank{enemy_type::kGrunt, "gru", "GRU"},
                            Bank{enemy_type::kKnight, "kni", "KNI"},
                            Bank{enemy_type::kMaggot, "mag", "MAG"},
                            Bank{enemy_type::kZombie, "zom", "ZOM"},
                            Bank{enemy_type::kPlague, "plague", "PLAG"},
                            Bank{enemy_type::kWolf, "wolf", "WLF"},
                            Bank{enemy_type::kIce, "ice", "ICE"},
                            Bank{enemy_type::kWorm, "worm", "WOR"},
                            Bank{enemy_type::kDog, "sked", "SKED"},
                            Bank{enemy_type::kSkeleton, "skel", "SKE"},
                            Bank{enemy_type::kDeath, "itemanim/hires", "DEATH"},
                            Bank{enemy_type::kIt, "it", "IT"}};

const Bank* bankFor(s32 kind) {
    const auto found = std::ranges::find(kBanks, kind, &Bank::kind);
    return found == kBanks.end() ? nullptr : &*found;
}

std::string action(std::span<const u8> program, usize index) {
    // e0070130/e00702b8 use this ordered 12-byte action table. Its native
    // FOURCC is at +8; the common dispatcher preserves the same 33 actions.
    constexpr usize kActions = 0xe00709a4U - 0xe0000400U;
    const usize at = kActions + index * 12 + 8;
    if (at > program.size() || program.size() - at < 4)
        throw FormatError("N64 enemy action table is unavailable");
    std::string result;
    for (usize i = 0; i < 4; ++i) {
        const u8 c = program[at + i];
        if (c != 0)
            result.push_back(static_cast<char>(c));
    }
    return result;
}
} // namespace

void translateLegendsEnemyAnimations(formats::AnimationFile& animation, s32 kind,
                                     std::span<const u8> program) {
    const auto* bank = bankFor(kind);
    if (!bank)
        throw FormatError("Unsupported N64 enemy species");
    if (kind == enemy_type::kIce) {
        // ICE2 STRT's packed vertex keys flatten its body to a ~0.27-unit
        // sheet for the whole emergence (native e0031f88 -> e0029d34 does
        // the same). The separate headlamp still rises normally. Repair this
        // source-art defect locally: keep REDY's first body mesh under STRT's
        // authored root translation/scale and lamp tracks. Do not change the
        // morph decoder or the other species' legitimate squash animations.
        const auto ready =
            std::ranges::find(animation.trees, "ICE2:REDY", &formats::TreeDefinition::name);
        const auto start =
            std::ranges::find(animation.trees, "ICE2:STRT", &formats::TreeDefinition::name);
        if (ready != animation.trees.end() && start != animation.trees.end()) {
            if (ready->nodes.size() != start->nodes.size())
                throw FormatError("N64 Ice emergence has incompatible native bodies");
            for (usize i = 0; i < start->nodes.size(); ++i) {
                auto& target = start->nodes[i];
                const auto& source = ready->nodes[i];
                if (target.objectFrames.empty())
                    continue;
                if (target.name != source.name || target.parent != source.parent ||
                    source.objectFrames.empty() || source.objectFrames.front().object.empty())
                    throw FormatError("N64 Ice emergence is missing its native body pose");
                target.object = source.objectFrames.front().object;
                target.objectFrames.clear();
            }
        }
    }
    std::array<std::string, 6> groups;
    for (usize i = 0; i < 3; ++i)
        groups[i] = std::format("{}{}", bank->prefix, i + 1);
    constexpr std::array kVariants{"C", "B", "S"};
    for (usize i = 0; i < kVariants.size(); ++i)
        groups[i + 3] = std::string(bank->prefix) + kVariants[i];
    if (kind == kItKind || kind == kDeathKind)
        groups[0] = bank->prefix;
    std::array<std::string, kEnemyActionCount> nativeActions;
    for (usize i = 0; i < nativeActions.size(); ++i)
        nativeActions[i] = action(program, i);
    if (nativeActions.front() != "REDY" || nativeActions.back() != "DETH")
        throw FormatError("Unexpected N64 enemy action table");
    // Native group membership does not imply a single BODY. e005ec28 switches
    // suicide bodies to a separate EXP debris hierarchy, and IT:SIGN is a
    // separate tag, not the roaming creature. Only e0070130's actual actions
    // belong to the common animator; preserve other native trees independently.
    std::vector<formats::TreeDefinition> separate;
    std::vector<formats::TreeDefinition> actions;
    for (auto& tree : animation.trees) {
        const auto colon = tree.name.find(':');
        const auto group = tree.name.substr(0, colon);
        const bool selected = std::ranges::find(groups, group) != groups.end();
        const bool action =
            tree.sequences.size() == 1 &&
            std::ranges::find(nativeActions, tree.sequences.front().name) != nativeActions.end();
        if (selected && !action) {
            // e005d734 creates DEATH/ARC from itemanim/generic while the
            // creature's actions come from hires. Expose that native effect
            // through the shared Death controller's effect identity.
            if (kind == kDeathKind && tree.name == "DEATH:ARC") {
                tree.name = "DEATH_ARC";
                // e005d734 calls e002fbfc with the local attachment offset
                // at e005fcd0, whether the drain is attached to Death or reversed.
                for (auto& node : tree.nodes)
                    if (node.parent < 0)
                        node.position += Vec3{.5f, .2f, 1.0f};
            }
            if (group == groups.back() && tree.sequences.size() == 1 &&
                tree.sequences.front().name == "EXP")
                tree.name = "SUICIDEEXP";
            separate.push_back(std::move(tree));
        } else {
            actions.push_back(std::move(tree));
        }
    }
    animation.trees = std::move(actions);
    std::array<std::string_view, 6> selected;
    std::ranges::copy(groups, selected.begin());
    formats::combineLegendsN64SequenceGroups(animation, selected);
    const auto common = enemyKind(kind).prefix;
    for (auto& tree : animation.trees) {
        const auto found = std::ranges::find(groups, tree.name);
        if (found == groups.end()) {
            // e005d838/e005d874 select these exact shared native ammo groups.
            if (tree.name == "GRUNTAMMO:AROW")
                tree.name = std::string(common) + "_ARROW";
            else if (tree.name == "GRUNTAMMO:BOMB")
                tree.name = std::string(common) + "_BOMB";
            continue;
        }
        const usize index = static_cast<usize>(found - groups.begin());
        constexpr std::array kCommonVariants{"A", "B", "S"};
        tree.name = index < 3 ? std::format("{}{}", common, index + 1)
                              : std::string(common) + kCommonVariants[index - 3];
        for (auto& sequence : tree.sequences) {
            for (usize i = 0; i < nativeActions.size(); ++i) {
                if (sequence.name == nativeActions[i]) {
                    sequence.name = EnemyAnimator::kSequenceNames[i];
                    break;
                }
            }
        }
    }
    for (auto& tree : separate)
        animation.trees.push_back(std::move(tree));
}

std::unique_ptr<ItemArchive> loadLegendsEnemyAssets(const std::filesystem::path& nativeRoot,
                                                    s32 kind, std::span<const u8> program,
                                                    std::vector<std::string>& warnings) {
    const auto* bank = bankFor(kind);
    if (!bank)
        return {};
    const auto directory = nativeRoot / bank->directory;
    std::vector<std::filesystem::path> adjuncts;
    if (kind == kDeathKind)
        adjuncts.push_back(nativeRoot / "itemanim/generic");
    // e006a640 loads the medium species' ex and l3 banks when present.
    // These are explicit siblings, never a search for unrelated replacement art.
    for (const auto child : {"l3", "ex"}) {
        if (AssetLocator(directory / child).find("seq.wad"))
            adjuncts.push_back(directory / child);
    }
    return loadLegendsN64Archive(directory, warnings, program, {}, false, adjuncts,
                                 [&](formats::AnimationFile& animation) {
                                     translateLegendsEnemyAnimations(animation, kind, program);
                                 });
}
} // namespace gdl::game
