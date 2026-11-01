#include <array>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "expansions/n64/assets/LegendsN64Archive.h"
#include "expansions/n64/bosses/LegendsLegend.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

std::optional<u32> legendsLegendSound(s32 index, LegendSound phase) {
    if (index < 0 || index >= 4)
        return {};
    // Boss.bin e088a638/b324/c150/cd48: all four reveal with0x9e.
    // The first three throw with0x39; Yeti's fireballs launch with0x122.
    // Sword impact calls the boss damage path rather than an extra macro.
    switch (phase) {
    case LegendSound::PickedUp: return 0x9e;
    case LegendSound::Thrown: return index == 1 ? 0x122 : 0x39;
    case LegendSound::Landed:
        switch (index) {
        case 0: return 0x3e;
        case 1: return 0x93;
        case 3: return 0x74;
        default: return {};
        }
    case LegendSound::Flying:
    case LegendSound::WornOff: return {};
    }
    return {};
}

bool LegendsLevel::playBossLegendSound(LegendSound phase) {
    if (platform() != LegendsPlatform::N64 || !m_boss || !m_bossLegend)
        return false;
    if (const auto token = legendsLegendSound(m_boss->consumedLegend, phase))
        m_opponentSounds.push_back(*token);
    // Native spinning whooshes were discrete cinematic rotations, not an
    // endlessly looping GDL flight sample. Unmapped phases remain silent.
    return true;
}

LegendsLegendBinding loadLegendsLegend(const std::filesystem::path& nativeRoot, s32 index,
                                       std::span<const u8> program,
                                       std::vector<std::string>& warnings) {
    if (index < 0 || index >= 4) {
        return {};
    }
    // e0889ca0 loads these exact libraries and named meshes for the four rites.
    // Their POWERUP trees contain the same authored weapon geometry. Retain its
    // native hierarchy/texture animation while the GDL hand/throw drives it.
    static constexpr std::array paths{"items/s/li", "items/s/lf", "items/s/ls", "items/s/lj"};
    static constexpr std::array trees{"POWERUP:QICE", "POWERUP:QFLM", "POWERUP:QSCM",
                                      "POWERUP:QJAV"};
    const std::array fireBanks{nativeRoot / "weapons", nativeRoot / "bosseffect/skodeath"};
    const std::span<const std::filesystem::path> lenders =
        index == 1 ? fireBanks : std::span<const std::filesystem::path>{};
    LegendsLegendBinding result;
    result.archive = loadLegendsN64Archive(nativeRoot / paths[static_cast<usize>(index)], warnings,
                                           program, {}, false, lenders);
    result.profile.held = trees[static_cast<usize>(index)];
    result.profile.projectile = result.profile.held;
    result.profile.flight = LegendShow::Flight::Flies;
    if (index == 2 || index == 3) {
        // chi TYPE child2 is BODY1_LION; slime TYPE.lookNode1 is
        // BODY1_EYEBALL. BNAM is the native skeletal identity, not the model's
        // suffix (the lion is morphing geometry). The common bound hierarchy
        // preserves this index through sequence composition.
        const auto names = formats::decodeLegendsN64BoneNames(
            readFile(nativeRoot / (index == 2 ? "chimera/seq.wad" : "slime/seq.wad")));
        result.profile.targetNode =
            formats::findLegendsN64Bone(names, index == 2 ? "BODY1_LION" : "BODY1_EYEBALL");
        if (!result.profile.targetNode)
            throw FormatError("Native legendary weapon target is unavailable");
        // Neither N64 archive has GDL's STUMPLQ/PBOSSQEYEBALL models. Keep
        // the native eye and cue identity, without inventing donor artwork.
        // Native STUMPL itself is an authored but absent effect group; its
        // separate 0x400 body-hide operation still removes the lion head.
    }
    if (index == 1) {
        // e088cd48 launches WEAPONS:FPRJ at the Yeti, then BOSSDEATHFX on hit.
        // Do not reuse the GameCube arena's hard-coded +85 Z flame placement.
        result.profile.projectile = "WEAPONS:FPRJ";
        result.profile.burst = "BOSSDEATHFX";
    }
    if (index == 3) {
        // e088c150 reparents the javelin to the boss's lookNode1 at255,
        // zeroes local XYZ, then deletes it at378 on its30Hz cinematic clock.
        result.profile.impactAttachmentSeconds = (378.f - 255.f) / 30.f;
    }
    if (!result.archive || !result.archive->trees.find(result.profile.held) ||
        !result.archive->trees.find(result.profile.projectile)) {
        throw FormatError("Native legendary weapon artwork is unavailable");
    }
    // The shared archive loader names standalone native groups group:FOURCC.
    // BOSSDEATHFX is a group; resolve its sole authored sequence, never alias an
    // unrelated flame to satisfy a missing name.
    if (!result.profile.burst.empty() && !result.archive->trees.find(result.profile.burst)) {
        for (usize i = 0; i < result.archive->trees.size(); ++i) {
            const auto& tree = result.archive->trees.tree(static_cast<u32>(i));
            if (tree.name.starts_with("BOSSDEATHFX:")) {
                if (result.profile.burst != "BOSSDEATHFX") {
                    throw FormatError("Ambiguous native legendary flame animation");
                }
                result.profile.burst = tree.name;
            }
        }
        if (result.profile.burst == "BOSSDEATHFX") {
            throw FormatError("Native legendary flame animation is unavailable");
        }
    }
    return result;
}

bool LegendsLevel::hasBossLegend(const CharacterSave& save) const {
    // Boss.bin e088ddf8 selects four stage-eight rites (realms3,6,0,7).
    // The fifth legendary item is not one of these rites; leave it intact.
    return save.character >= 0 && save.character < kClassCount &&
           platform() == LegendsPlatform::N64 && m_boss && m_boss->consumedLegend >= 0 &&
           m_boss->consumedLegend < 4 &&
           save.progress()
               .legendsProgress(LegendsPlatform::N64)
               .has(LegendsRelic::LegendItem, m_boss->consumedLegend);
}

bool LegendsLevel::consumeBossLegend(CharacterSave& save) {
    if (!hasBossLegend(save)) {
        return false;
    }
    save.progress().legendsProgress(LegendsPlatform::N64).legendItems &=
        ~(u64{1} << static_cast<u32>(m_boss->consumedLegend));
    return true;
}

} // namespace gdl::game
