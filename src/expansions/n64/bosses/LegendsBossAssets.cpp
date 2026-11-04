#include "expansions/n64/bosses/LegendsBossAssets.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "expansions/n64/assets/LegendsN64Archive.h"
#include "expansions/n64/audio/LegendsSoundTable.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "formats/AnimationTree.h"
#include "game/bosses/BossDefinition.h"
#include "game/combat/EffectFlags.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/BossKinds.h"

namespace gdl::game {
namespace {
struct Bank {
    s32 kind;
    std::string_view data;
    std::string_view art;
};
// Native DESC names and Boss.bin's six level-family bindings. Plague's boss
// bank is SLIME; PLAGUE contains the ordinary PLAG1/2/3 minions.
constexpr std::array kBanks{
    Bank{boss_type::kDragon, "dragon", "dragon"},   Bank{boss_type::kChimera, "chi", "chimera"},
    Bank{boss_type::kPlagueBoss, "slime", "slime"}, Bank{boss_type::kYeti, "yeti", "yeti"},
    Bank{boss_type::kSkorne1, "sko1", "boss1"},     Bank{boss_type::kSkorne2, "sko2", "boss2"}};

void alignSkorneRoar(formats::AnimationFile& animation) {
    const auto found = animation.find("BOSSROARFX");
    if (!found)
        throw FormatError("N64 Skorne: missing roar effect");
    auto& tree = animation.trees[*found];
    constexpr s16 kPreRoll = 20;
    constexpr s16 kNativeFrames = 38;
    if (tree.sequences.size() != 1 || tree.sequences.front().frameCount != kNativeFrames ||
        tree.sequences.front().frameRate != 30)
        throw FormatError("N64 Skorne: unexpected roar effect clock");
    // Deliberate GDL compatibility adjustment, not N64 playback parity. Both
    // games cue ROAR at move frame20. N64 BOSSROARFX then waits another20
    // frames (e0032478); the GC counterpart instead contains only the final
    // 17/18 frames, with zero texture offsets and the same ROAR*20 sources.
    // Retain every native visible sample, but begin that phase at the cue so
    // the effect accompanies the stretch instead of appearing after it.
    auto& sequence = tree.sequences.front();
    sequence.frameCount = kNativeFrames - kPreRoll;
    for (auto& track : sequence.tracks) {
        if (track.frames.size() == 1 && track.frames.front() == 0)
            continue;
        if (track.frames.size() != kNativeFrames ||
            track.values.size() != usize{kNativeFrames} * track.channelCount())
            throw FormatError("N64 Skorne: unexpected roar transform samples");
        for (usize i = 0; i < track.frames.size(); ++i)
            if (track.frames[i] != i)
                throw FormatError("N64 Skorne: sparse roar transform samples");
        track.frames.erase(track.frames.begin(), track.frames.begin() + kPreRoll);
        track.values.erase(track.values.begin(),
                           track.values.begin() + kPreRoll * track.channelCount());
        for (auto& frame : track.frames)
            frame -= kPreRoll;
    }
    for (auto& node : tree.nodes) {
        for (auto& run : node.objectFrames) {
            if (run.frames <= 1)
                continue;
            if (run.start != 0 || run.frames != kNativeFrames)
                throw FormatError("N64 Skorne: unexpected roar model samples");
            // A negative run origin selects native sample20 at effect frame0;
            // no model/texture pixels or shared archive slots are rewritten.
            run.start = -kPreRoll;
        }
    }
}
} // namespace
std::unique_ptr<CombatantAssets> loadLegendsBossAssets(RenderDevice& device,
                                                       const std::filesystem::path& nativeRoot,
                                                       const std::filesystem::path& gameCubeRoot,
                                                       s32 kind, std::span<const u8> program,
                                                       std::span<TextureSet* const> textureLenders,
                                                       std::vector<std::string>& warnings) {
    const Bank* bank = nullptr;
    for (const auto& candidate : kBanks)
        if (candidate.kind == kind)
            bank = &candidate;
    if (bank == nullptr)
        return {};
    const AssetLocator files{nativeRoot / bank->data};
    const auto path = files.find("moves.wad");
    if (!path)
        throw FileError("N64 Legends boss moves.wad is missing: " + std::string(bank->data));
    auto decoded = formats::decodeLegendsN64Critter(readFile(*path));
    if (kind == boss_type::kSkorne1) {
        const auto first =
            std::ranges::find(decoded.moves, "START", &formats::CritterMoveRecord::name);
        const auto second =
            std::ranges::find(decoded.moves, "START2", &formats::CritterMoveRecord::name);
        if (first == decoded.moves.end() || second == decoded.moves.end() ||
            first->anim != "STRT" || second->anim != "STR2" || first->link != -1 ||
            first->type != 0x10 || second->type != 0xf0 || second->link != -1)
            throw FormatError("N64 Skorne: unexpected entrance move table");
        // Intentional GDL compatibility, not a recovered N64 link. GDL SKORNE1
        // chains its 51-frame START to the 83-frame START2 at30fps. N64 retains
        // those same clips but leaves STR2 outside autonomous move selection.
        // Preserve both native clips and STR2's frame26 cue; START's stale cue76
        // remains unreachable rather than being replayed across the boundary.
        first->link = static_cast<s16>(second - decoded.moves.begin());
        second->type = 0x10;
        second->priority = 0xfff;
        second->interrupt = 0;
        for (const auto& [name, frame] :
             {std::pair{"POUNDL", s32{10}}, std::pair{"POUNDR", s32{9}}}) {
            const auto pound =
                std::ranges::find(decoded.moves, name, &formats::CritterMoveRecord::name);
            if (pound == decoded.moves.end() || pound->type != 0x82 || pound->frameStart != 4 ||
                pound->frameEnd != -1 || pound->damage0 < 0 ||
                pound->anim != (name == std::string_view{"POUNDL"} ? "AT6" : "AT7"))
                throw FormatError("N64 Skorne: unexpected ground-punch move table");
            // GDL compatibility repair: GC STOMP emits its detached ground wave
            // at frame9, not the N64 table's frame4 wind-up. The native clips
            // first put the wrists on the arena at L10/R9; frame4 creates the
            // wave behind both authored target cones, above the central hole.
            // e0882e10/e0881b84 snapshot that position once, so it cannot follow
            // the descending fist. Retain the clips, move targets and damage;
            // emit their sound/visual/damage together at the actual slam pose.
            pound->frameStart = frame;
        }
    }
    if (kind == boss_type::kSkorne2) {
        // Intentional imported-gameplay correction: native e08832b8 advances
        // an admitted pattern without testing each move's target again. SKO2
        // PAT1/PAT2 admit any distance >=25, yet include a claw/grab capped at 25.
        // Keep their ranged attacks and cadence, but leave close attacks to
        // their standalone target gates instead of swinging across the arena.
        for (auto& pattern : decoded.patterns) {
            auto write = pattern.moves.begin();
            for (const s16 index : pattern.moves) {
                if (index < 0)
                    break;
                const auto& move = decoded.moves.at(static_cast<usize>(index));
                if (move.name != "CLAWL" && move.name != "GRAB")
                    *write++ = index;
            }
            std::fill(write, pattern.moves.end(), s16{-1});
        }
    }
    const auto sounds = decodeLegendsSoundTable(program);
    std::set<std::string> silentSounds;
    for (auto& effect : decoded.sounds) {
        if (kind == boss_type::kChimera && effect.name == "SUPERFIREFX") {
            const auto index = static_cast<s32>(&effect - decoded.sounds.data());
            const formats::CritterDamageRecord* source = nullptr;
            for (const auto& damage : decoded.damages) {
                if (damage.sfxIndex != index)
                    continue;
                if (damage.type != 3 || (source && damage.offset != source->offset))
                    throw FormatError("N64 Chimera: ambiguous compound breath placement");
                source = &damage;
            }
            if (!source || effect.flags != effect_flags::kRootParent ||
                std::ranges::any_of(decoded.moves, [index](const auto& move) {
                    return move.sfx == index || move.sfx2 == index;
                }))
                throw FormatError("N64 Chimera: unexpected compound breath cue");
            // Boss.bin e08824e8 -> e0881e34 -> e0881b84 adds the ring's
            // DAMG offset to SFXX for native bit0x800. Only bit1 replaces it.
            // Common GDL startArea instead replaces DAMG for both root modes.
            // Fold this compound cue's authored -10Y/+10Y cancellation into
            // the adapter, retaining its parent, complete three-head artwork,
            // animation clock and damage data. Single-head cues are unchanged.
            for (usize axis = 0; axis < effect.offset.size(); ++axis)
                effect.offset[axis] += source->offset[axis];
        }
        if (kind == boss_type::kPlagueBoss && (effect.flags & effect_flags::kBodyParent) != 0) {
            // e0881474 creates an identity animation container at actor+0x7c,
            // then e002f278 adds the BODY hierarchy below it. e0881a28's bit1
            // attaches effects to that container, not BODY's first node. Plague's
            // TOP carries Y=151/64; parenting to TOP lifted its gas (and the other
            // body-root cues) a second time. The common root-parent mode addresses
            // that missing container without altering the authored SFXX offsets.
            effect.flags = (effect.flags & ~effect_flags::kBodyParent) | effect_flags::kRootParent;
        }
        if (kind == boss_type::kSkorne1 && effect.levelFormat == "S_SKORN1ROAR" &&
            !legendsSoundToken(sounds, effect.levelFormat) &&
            legendsSoundToken(sounds, "S_SKORN1PROAR")) {
            // Compatibility repair, not bit-exact parity: the shipped ROAR and
            // PATROAR SFXX spelling never resolves in e007a408. The native sound
            // table owns the Skorne1 roar under PROAR. Preserve its frame20 cue
            // and original native audio rather than borrowing a GameCube bank.
            effect.levelFormat = "S_SKORN1PROAR";
        }
        if (kind == boss_type::kSkorne2) {
            // SKO2's move file disagrees with three native sound-table names:
            // rows 50/51 spell this boss SKORN9, and row 55 calls its roar PROAR.
            // Repair only these evidenced spellings, preserving their native
            // samples and cue frames. Other absent names remain silent below.
            constexpr std::array aliases{std::pair{"S_SKORN2ATTCK9", "S_SKORN9ATTCK9"},
                                         std::pair{"S_SKORN2ATTCK10", "S_SKORN9ATTCK10"},
                                         std::pair{"S_SKORN2ROAR", "S_SKORN2PROAR"}};
            for (const auto& [authored, routed] : aliases) {
                if (effect.levelFormat == authored && !legendsSoundToken(sounds, authored) &&
                    legendsSoundToken(sounds, routed))
                    effect.levelFormat = routed;
            }
        }
        // game.bin e0074d28/e0074b4c pass their 3.6-unit electric/acid
        // explosion radius to e0077064, which scales the tree by radius/5.
        if (effect.name == "EBALL_EXP" || effect.name == "ABALL_EXP") {
            effect.scale = 0.72f;
            effect.impactRadius = 3.6f;
            effect.impactPhysicalFraction = effect.name == "ABALL_EXP" ? 0.1f : 0.2f;
            effect.impactElementalFraction = effect.name == "ABALL_EXP" ? 0.9f : 0.8f;
            effect.impactElement = effect.name == "ABALL_EXP" ? 4U : 2U;
        } else if (effect.name == "FBALL_EXP" || effect.name == "FBALL_EXP2") {
            // e007496c rounds these two contributions separately; they do
            // not form a complementary split of one rounded damage total.
            effect.impactRadius = 5;
            effect.impactPhysicalFraction = 0.66f;
            effect.impactElementalFraction = 0.33f;
            effect.impactElement = 1;
        }
        if (!effect.levelFormat.empty() && !legendsSoundToken(sounds, effect.levelFormat)) {
            // e0880320 caches e007a408's -1 for absent names; e0881b84 still
            // runs the visual/link but skips audio. Several shipped HITFAR,
            // roar and attack strings have no native cue. Do not substitute
            // the similarly named GameCube sounds or disable the visual.
            if (silentSounds.insert(effect.levelFormat).second) {
                warnings.push_back("N64 " + std::string(bank->data) + ": native sound " +
                                   effect.levelFormat + " has no routing and is silent in retail");
            }
            effect.levelFormat.clear();
        }
    }
    // game.bin e006a858 loads these exact boss-effect banks by native realm.
    // No global archive search or GameCube projectile replacement is permitted.
    std::vector<std::filesystem::path> effects;
    const auto add = [&](std::string_view name) {
        effects.push_back(nativeRoot / "bosseffect" / name);
    };
    switch (kind) {
    case boss_type::kDragon: add("fire"); break;
    case boss_type::kChimera:
        add("fire");
        add("acid");
        add("elec");
        break;
    case boss_type::kPlagueBoss: add("acid"); break;
    case boss_type::kSkorne1:
        add("acid");
        add("elec");
        break;
    case boss_type::kSkorne2: add("fire"); break;
    default: break;
    }
    // e006b110 loads this native bank before the level-specific boss banks.
    // FBALL_EXP/FBALL_EXP2 live here; bosseffect/fire owns only FBALL_LOOP.
    // Keep the boss's own groups authoritative, with this explicit shared
    // lender last rather than falling back to GameCube weapon effects.
    effects.push_back(nativeRoot / "weapons");
    auto archive =
        loadLegendsN64Archive(nativeRoot / bank->art, warnings, program, {}, true, effects,
                              [kind](formats::AnimationFile& animation) {
                                  if (kind == boss_type::kSkorne1 || kind == boss_type::kSkorne2) {
                                      alignSkorneRoar(animation);
                                  }
                              });
    if (!archive)
        throw FileError("N64 Legends boss objects.wad is missing: " + std::string(bank->art));
    const auto family = bossDefinition(bossNameOf(kind));
    const auto sequencePath = AssetLocator(nativeRoot / bank->art).find("seq.wad");
    if (!sequencePath)
        throw FileError("N64 Legends boss seq.wad is missing: " + std::string(bank->art));
    const auto boneNames = formats::decodeLegendsN64BoneNames(readFile(*sequencePath));
    const auto& primary = decoded.types.front();
    const auto& descriptor = decoded.descriptors[static_cast<usize>(primary.descriptorIndex)];
    const auto bodyIndex = archive->trees.find(descriptor.prefix + primary.suffix);
    if (!bodyIndex)
        throw FormatError("N64 Legends boss: missing primary body");
    const auto& body = archive->trees.tree(*bodyIndex);
    const auto bindNode = [&](std::string& name) {
        if (name.empty())
            return;
        const auto index = formats::findLegendsN64Bone(boneNames, name);
        if (!index || *index >= body.nodes.size())
            throw FormatError("N64 Legends boss: missing native BNAM node " + name);
        // Combat uses the sequence's BNAM table, not the OBHD suffix used by
        // drawable attachment lookup. Preserve its explicit node identity when
        // translating into common TreeInfo names (notably RESETBODY1_L_WR).
        name = body.nodes[*index].name;
        if (body.findNode(name, kCombatantNodeNameLength) != index)
            throw FormatError("N64 Legends boss: ambiguous translated node " + name);
    };
    for (auto& move : decoded.moves)
        bindNode(move.colnode);
    for (auto& node : decoded.nodes)
        bindNode(node.nodeName);
    for (auto& type : decoded.types) {
        bindNode(type.rootNode);
        bindNode(type.lookNode0);
        bindNode(type.lookNode1);
        bindNode(type.noSkinNode);
    }
    // Boss.bin e0881e34 / e08882a0: the two-second rate interval and four-unit
    // lowered ballistic aim differ from GDL. game.bin e0078270 grows area
    // cylinders linearly at full damage; e08824b4 sets their half-height to50.
    CritterRules rules{.projectileMaxRate = 2.5f,
                       .projectileRateScale = 0.5f,
                       .ballisticTargetOffset = -4,
                       .linearRadialAreas = true,
                       .areaHalfHeight = 50,
                       .pressureReferenceDamage = primary.pressureReferenceDamage,
                       .nativeImpactAreas = true,
                       .worldAxisBreathPitch = true,
                       // e08819b8 targets feet+4 before e08882a0 subtracts four.
                       // A class-dependent midpoint would aim below the floor.
                       .projectileTargetHeight = 4,
                       .restoreCaptureSupport = true};
    auto data = CritterData::fromDecoded(family.name, decoded, 0, rules);
    std::vector<CritterData> children;
    std::set<s32> visited{0};
    for (s32 child = data.childIndex(); child >= 0;) {
        if (!visited.insert(child).second || static_cast<usize>(child) >= decoded.types.size()) {
            throw FormatError("N64 Legends boss: cyclic or invalid child type");
        }
        rules.pressureReferenceDamage =
            decoded.types[static_cast<usize>(child)].pressureReferenceDamage;
        auto part =
            CritterData::fromDecoded(family.name, decoded, static_cast<usize>(child), rules);
        child = part.childIndex();
        children.push_back(std::move(part));
    }
    auto result = std::make_unique<CombatantAssets>();
    if (!result->bindDecoded(device, gameCubeRoot, family, 0, std::move(*archive), std::move(data),
                             std::move(children), textureLenders)) {
        throw FormatError("N64 Legends boss: native body/data binding failed: " + family.name);
    }
    const auto validateMoves = [&](const CritterData& part) {
        for (const auto& move : part.moves()) {
            if (!result->tree->findSequence(move.anim)) {
                throw FormatError("N64 Legends boss: missing native move sequence " + part.tree() +
                                  ":" + move.anim);
            }
            if (!move.colnode.empty() &&
                !result->tree->findNode(move.colnode, kCombatantNodeNameLength)) {
                throw FormatError("N64 Legends boss: missing native attack node " + move.colnode);
            }
        }
    };
    validateMoves(result->data);
    for (const auto& child : result->children)
        validateMoves(child);
    std::set<std::string> missingEffects;
    for (const auto& cue : result->data.sounds()) {
        if (cue.shows() && !result->archive.trees.find(cue.tree) &&
            missingEffects.insert(cue.tree).second) {
            warnings.push_back("N64 " + family.name + ": native effect group " + cue.tree +
                               " is absent from the authored banks; no replacement is used");
        }
    }
    // Boss.bin e0882a50 stamps the beginning of a move; e0881e34 assigns moving
    // effect attacks 750 ticks on the native 150 Hz lifetime clock.
    result->cooldownStartsWithMove = true;
    result->projectileLifetime = 5;
    // Boss.bin e08842e8 arms its wake deadline four seconds after placement.
    result->wakeDelay = 4;
    // Boss e0882f9c keeps state 0 in INIT, then requests START followed by READY.
    // INIT contains the authored initial body/morph pose, so holding START's
    // first pose during the wake delay is not interchangeable.
    result->initialMoveBeforeWake = true;
    return result;
}
} // namespace gdl::game
