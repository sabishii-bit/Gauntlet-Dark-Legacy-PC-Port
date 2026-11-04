#include "game/bosses/BossTraits.h"

#include <algorithm>
#include <array>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/bosses/LegendItems.h"
#include "game/level/BossKinds.h"
#include "game/players/PlayerDeeds.h"

namespace gdl::game {
namespace {

using Flight = LegendShow::Flight;

/** One row a boss, in code order.
 * The spider's egg impact leaves its L1 mesh at full size in the final pose, and the lich has
 * a separate generator tree whose ACTIVE sequence repeats; neither archive exposes these
 * bodies under the generic BOSSGEN name.
 * PlayerMotion reads the bosses' item offsets from their pool entries with lfd, not lfs;
 * reading only their high words puts the stationary effects close to the boss's feet.
 * The defeats are DoGoodWizard's (auxscreen.c 236); only the eight realms' guardians leave a
 * key. */
constexpr std::array<BossTraits, kBossTypeCount> kTraits{{
    {.kind = boss_type::kDragon,
     .legend = {.strike = LegendStrike::AtRiteEnd,
                .darkUntilImpact = true,
                .heldInHand = true,
                .gesture = PlayerDeed::ThrowLegend,
                .flight = Flight::Flies,
                .trailTexture = "PARTICLE1_A",
                .frozenSkin = "SEETHROUGH"},
     .defeat = {.speech = "DRAGON_SPEECH", .dropsKey = true}},
    {.kind = boss_type::kChimera,
     .legend = {.quickToRoar = true,
                .strike = LegendStrike::OnLanding,
                .heldInHand = true,
                .gesture = PlayerDeed::ThrowLegend,
                .flight = Flight::Flies,
                .trailTexture = "CHIMKEY_PART",
                .targetNode = "CHIMLIONHEA",
                .lionStump = "STUMPLQ"},
     .defeat = {.speech = "CHIMERA_SPEECH", .dropsKey = true}},
    {.kind = boss_type::kDjinn,
     .legend = {.strike = LegendStrike::AtRiteEnd,
                .heldInHand = true,
                .gesture = PlayerDeed::ShootLegend,
                .flight = Flight::Flies,
                .burstFollowSeconds = LegendShow::kGenieBurstSeconds},
     .defeat = {.speech = "DJINN_SPEECH", .dropsKey = true}},
    {.kind = boss_type::kDrider,
     .generator = {.tree = "GENPROJHIT", .settled = true},
     .legend = {.strike = LegendStrike::OnLanding,
                .landsOnRelease = true,
                .heldInHand = true,
                .gesture = PlayerDeed::ShootLegend,
                .flight = Flight::WithBearer,
                .burstSeconds = LegendShow::kSpiderBurstSeconds,
                .strikeTint = Color::rgba(64, 255, 64)}, // the spider, poisoned
     .defeat = {.speech = "DRIDER_SPEECH", .dropsKey = true}},
    {.kind = boss_type::kPlagueBoss,
     .legend = {.strike = LegendStrike::OnLanding,
                .landsWhileRiteRuns = true,
                .heldInHand = true,
                .gesture = PlayerDeed::ThrowLegend,
                .flight = Flight::Flies,
                .homes = true,
                .trailTexture = "PARTICLE1_A",
                .targetNode = "BODY1_EYEBALL",
                .eyeReplacement = "PBOSSQEYEBALL",
                .restoresSight = true},
     .defeat = {.speech = "PBOSS_SPEECH", .runeVoices = RuneVoices::OwnAll, .dropsKey = true}},
    {.kind = boss_type::kYeti,
     .projectileEndVisual = true,
     .legend = {.heldInHand = true,
                .gesture = PlayerDeed::ThrowLegend,
                .restingTree = LegendShow::kBurstTree,
                .burstTree = LegendShow::kSecondBurstTree,
                .bossOffset = Vec3{-7.0f, -5.0f, 85.0f}},
     .defeat = {.speech = "YETI_SPEECH", .runeVoices = RuneVoices::OwnAll, .dropsKey = true}},
    {.kind = boss_type::kWraith,
     .legend = {.bossOffset = Vec3{0.0f, -5.0f, 10.0f}},
     .defeat = {.speech = "WRAITH_SPEECH", .runeVoices = RuneVoices::OwnAll, .dropsKey = true}},
    {.kind = boss_type::kLich,
     .generator = {.tree = "ATK12GEN", .settled = false},
     .legend = {.quickToRoar = true, .burstSeconds = LegendShow::kLichBurstSeconds},
     .defeat = {.speech = "LICH_SPEECH", .runeVoices = RuneVoices::OwnAll, .dropsKey = true}},
    {.kind = boss_type::kSkorne1,
     .legend = {.quickToRoar = true,
                .strike = LegendStrike::OnLanding,
                .landsOnRelease = true,
                .bossOffset = Vec3{0.0f, 17.0f, 3.0f}},
     .defeat = {.speech = "SKORNE1_SPEECH",
                .defeatVoice = "S_E2VOXA",
                .runeVoices = RuneVoices::Temple,
                .followUp = VictoryFollowUp::TempleRunes,
                .longWizardWait = true,
                .wizardAtAltar = true,
                .spew = BossSpew::Relics}},
    {.kind = boss_type::kSkorne2,
     .defeat = {.speech = "SKORNE2_SPEECH",
                .defeatVoice = "S_ENDVOX",
                .runeVoices = RuneVoices::None,
                .followUp = VictoryFollowUp::Leaves,
                .wizardAtAltar = true}},
    {.kind = boss_type::kGarm,
     .defeat = {.speech = "GARM2_SPEECH",
                .defeatVoice = "S_GRMDESTVOX",
                .runeVoices = RuneVoices::None,
                .followUp = VictoryFollowUp::Leaves,
                .longWizardWait = true}},
}};
static_assert(
    [] {
        for (usize i = 0; i < kTraits.size(); ++i) {
            if (kTraits[i].kind != kFirstBossType + static_cast<s32>(i)) {
                return false;
            }
        }
        return true;
    }(),
    "each boss's traits sit at its code's row");

/** The staging of a code without a row. */
constexpr LegendStaging kNoStaging{};

} // namespace

const BossTraits* bossTraitsOf(s32 code) {
    if (!isBossType(code)) {
        return nullptr;
    }
    return &kTraits[static_cast<usize>(code - kFirstBossType)];
}

const BossTraits& orderedBossTraitsOf(s32 code) {
    const s32 nearest = std::clamp(code, kFirstBossType, kLastBossType);
    return kTraits[static_cast<usize>(nearest - kFirstBossType)];
}

const LegendStaging& legendStagingOf(s32 code) {
    const BossTraits* traits = bossTraitsOf(code);
    return traits != nullptr ? traits->legend : kNoStaging;
}

} // namespace gdl::game
