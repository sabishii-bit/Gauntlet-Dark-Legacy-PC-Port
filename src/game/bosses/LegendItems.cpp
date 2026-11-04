#include "game/bosses/LegendItems.h"

#include <algorithm>
#include <array>

#include "engine/core/Types.h"

#include "game/bosses/BossTraits.h"

namespace gdl::game {

namespace {

constexpr f32 kTenth = 0.1f;
constexpr f32 kWearSeconds = 29.0f;
constexpr f32 kTicksPerSecond = 60.0f;

/** The original's table, boss by boss. */
constexpr std::array<LegendWeakness, 9> kWeaknesses{{
    // boss, realm, share, damage, frozen, blind, curb, lasts, scale, beheads
    {34, 2, kTenth, 0.0f, 1200, 0, 0.0f, 0.0f, 1.0f, false},           // the dragon, iced
    {35, 1, 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, true},                 // the chimera, beheaded
    {36, 3, kTenth, 0.0f, 0, 1800, 0.0f, 0.0f, 1.0f, false},           // the genie, in the dark
    {37, 4, kTenth, 0.0f, 0, 0, 0.5f, 0.0f, 0.8f, false},              // the spider, poisoned
    {38, 11, kTenth, 0.0f, 0, 18000, 0.0f, kWearSeconds, 1.0f, false}, // restored by world_update
    {39, 9, kTenth, 0.0f, 0, 0, 0.5f, kWearSeconds, 1.0f, false},      // the yeti, melted
    {40, 10, 0.0f, 500.0f, 0, 0, 0.25f, kWearSeconds, 1.0f, false},    // the wraith, shown
    {41, 7, 0.25f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, false},               // the lich, burned
    {42, 5, kTenth, 0.0f, 0, 0, 0.1f, kWearSeconds, 1.0f, false},      // the temple's, shaken
}};

} // namespace

const LegendWeakness* legendWeaknessOf(s32 kind) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kWeaknesses, kind, &LegendWeakness::boss);
    return found != kWeaknesses.end() ? &*found : nullptr;
}

std::string_view LegendShow::chargeTree(s32 color) {
    constexpr std::array<std::string_view, 4> kTrees{"COMBO_YEL", "COMBO_BLU", "COMBO_RED",
                                                     "COMBO_GRN"};
    return kTrees[static_cast<usize>(std::clamp(color, 0, 3))];
}

Color LegendShow::chargeTint(s32 color) {
    constexpr std::array<Color, 4> kTints{Color::rgba(255, 255, 0), Color::rgba(0, 0, 255),
                                          Color::rgba(255, 0, 0), Color::rgba(0, 255, 0)};
    return kTints[static_cast<usize>(std::clamp(color, 0, 3))];
}

ParticleDescriptor LegendShow::trailOf(s32 kind) {
    ParticleDescriptor trail;
    const std::string_view texture = legendStagingOf(kind).trailTexture;
    if (texture.empty()) {
        return trail;
    }
    trail.texture = texture;
    trail.emitFrames = 90; // three seconds, then 0.034 seconds of emission fade
    trail.fadeFrames = 1;
    trail.angle = ParticleDescriptor::kSphere;
    trail.rate = {1.0f, 1.0f, 1.0f, 1.0f}; // thirty particles a second
    trail.speed = 1.0f / ParticleDescriptor::kFrameRate;
    trail.particleLife = 30;
    trail.particleFade = 30;
    trail.red = trail.green = trail.blue = {255.0f, 255.0f, 255.0f, 255.0f};
    trail.alpha = {255.0f, 255.0f, 255.0f, 0.0f};
    trail.width = {2.0f, 2.0f, 2.0f, 2.0f};
    trail.depthWrite = false;
    return trail;
}

s32 legendRealmOf(s32 kind) {
    const LegendWeakness* weakness = legendWeaknessOf(kind);
    return weakness != nullptr ? weakness->realm : 0;
}

void LegendRite::begin(s32 player, const LegendWeakness& weakness) {
    clear();
    m_stage = Stage::Carried;
    m_player = player;
    m_weakness = weakness;
}

void LegendRite::clear() {
    m_stage = Stage::None;
    m_weakness = {};
    m_player = -1;
    m_ticks = 0;
    m_roarDue = false;
    m_roared = false;
    m_brandished = false;
    m_thrown = false;
    m_wearLeft = 0.0f;
}

std::vector<LegendCue> LegendRite::update(s32 ticks, bool bossRisen, bool bossRoarDone) {
    std::vector<LegendCue> cues;
    if (!running() || ticks <= 0) {
        return cues;
    }
    if (m_stage == Stage::Carried) {
        if (!bossRisen) {
            return cues;
        }
        m_stage = Stage::Woken;
        m_ticks = 0;
    }
    m_ticks += ticks;
    // The bearer holds the item up as the boss rises, and throws it a second on.
    if (!m_brandished) {
        m_brandished = true;
        cues.push_back(LegendCue::Brandished);
    }
    if (!m_thrown && m_ticks >= kBrandishTicks) {
        m_thrown = true;
        m_stage = Stage::Struck;
        cues.push_back(LegendCue::Thrown);
    }
    // The boss roars at it, once its wait is over.
    const s32 roarWait = LegendShow::quickToRoar(m_weakness.boss) ? kShortRoarWait : kLongRoarWait;
    if (!m_roarDue && m_ticks >= roarWait) {
        m_roarDue = true;
    }
    if (m_roarDue && !m_roared && bossRoarDone) {
        m_roared = true;
        cues.push_back(LegendCue::Roared);
        m_wearLeft = m_weakness.recoverySeconds;
    }
    // Then its weakness runs its course, where it has one.
    if (m_roared && m_thrown && LegendShow::strikeOf(m_weakness.boss) != LegendStrike::AtRiteEnd) {
        if (m_weakness.recoverySeconds <= 0.0f) {
            m_stage = Stage::Over;
        } else {
            m_wearLeft -= static_cast<f32>(ticks) / kTicksPerSecond;
            if (m_wearLeft <= 0.0f) {
                m_stage = Stage::Over;
                cues.push_back(LegendCue::WornOff);
            }
        }
    }
    return cues;
}

bool LegendRite::finishOnImpact() {
    if (LegendShow::strikeOf(m_weakness.boss) != LegendStrike::AtRiteEnd ||
        m_stage != Stage::Struck) {
        return false;
    }
    m_stage = Stage::Over;
    m_roarDue = false;
    return true;
}

bool LegendRite::darkens() const {
    return running() && m_stage != Stage::Carried &&
           (LegendShow::darkUntilImpact(m_weakness.boss) || !m_roared);
}

bool LegendShow::heldInHand(s32 kind) {
    return legendStagingOf(kind).heldInHand;
}

PlayerDeed LegendShow::gestureOf(s32 kind) {
    return legendStagingOf(kind).gesture;
}

LegendShow::Flight LegendShow::flightOf(s32 kind) {
    return legendStagingOf(kind).flight;
}

std::string_view LegendShow::restingTreeOf(s32 kind) {
    return legendStagingOf(kind).restingTree;
}

std::string_view LegendShow::burstTreeOf(s32 kind) {
    return legendStagingOf(kind).burstTree;
}

f32 LegendShow::burstSecondsOf(s32 kind) {
    return legendStagingOf(kind).burstSeconds;
}

Vec3 LegendShow::bossOffsetOf(s32 kind) {
    return legendStagingOf(kind).bossOffset;
}

LegendStrike LegendShow::strikeOf(s32 kind) {
    return legendStagingOf(kind).strike;
}

bool LegendShow::landsWhileRiteRuns(s32 kind) {
    return legendStagingOf(kind).landsWhileRiteRuns;
}

bool LegendShow::landsOnRelease(s32 kind) {
    return legendStagingOf(kind).landsOnRelease;
}

bool LegendShow::quickToRoar(s32 kind) {
    return legendStagingOf(kind).quickToRoar;
}

bool LegendShow::darkUntilImpact(s32 kind) {
    return legendStagingOf(kind).darkUntilImpact;
}

bool LegendShow::restoresSight(s32 kind) {
    return legendStagingOf(kind).restoresSight;
}

bool LegendShow::homes(s32 kind) {
    return legendStagingOf(kind).homes;
}

std::string_view LegendShow::eyeReplacementOf(s32 kind) {
    return legendStagingOf(kind).eyeReplacement;
}

std::optional<Color> LegendShow::strikeTintOf(s32 kind) {
    return legendStagingOf(kind).strikeTint;
}

std::string_view LegendShow::lionStumpOf(s32 kind) {
    return legendStagingOf(kind).lionStump;
}

std::string_view LegendShow::targetNodeOf(s32 kind) {
    return legendStagingOf(kind).targetNode;
}

std::string_view LegendShow::frozenSkinOf(s32 kind) {
    return legendStagingOf(kind).frozenSkin;
}

f32 LegendShow::burstFollowSecondsOf(s32 kind) {
    return legendStagingOf(kind).burstFollowSeconds;
}

/** The common bank's pickup, then the realm's own: thrown, flying (`FLY`, or the `ALL`
 * loop of the items set on the boss), landed (`HIT`, or `ALSTP` for those), and worn off.
 * The dream's throw is misspelt in the bank. */
std::vector<std::string> LegendShow::soundNamesOf(Sound sound, char realm) {
    const std::string stem = std::string("S_") + realm + "LEGW";
    switch (sound) {
    case Sound::PickedUp: return {"S_LEGWPUP"};
    case Sound::Thrown: return {stem + "THROW", std::string("S_") + realm + "EGWTHROW"};
    case Sound::Flying: return {stem + "FLY", stem + "ALL"};
    case Sound::Landed: return {stem + "HIT", stem + "ALSTP"};
    case Sound::WornOff: return {stem + "PDN"};
    }
    return {};
}

} // namespace gdl::game
