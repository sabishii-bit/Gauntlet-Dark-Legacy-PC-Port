#include "game/enemies/LegendItems.h"

#include <algorithm>
#include <array>

namespace gdl::game {

namespace {

constexpr f32 kTenth = 0.1f;
constexpr f32 kWearSeconds = 29.0f;
constexpr f32 kTicksPerSecond = 60.0f;

/** The original's table, boss by boss. */
constexpr std::array<LegendWeakness, 9> kWeaknesses{{
    // boss, realm, share, damage, frozen, blind, curb, lasts, scale, beheads
    {34, 2, kTenth, 0.0f, 1200, 0, 0.0f, 0.0f, 1.0f, false},       // the dragon, iced
    {35, 1, 1.0f / 3.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, true},      // the chimera, beheaded
    {36, 3, kTenth, 0.0f, 0, 1800, 0.0f, 0.0f, 1.0f, false},       // the genie, in the dark
    {37, 4, kTenth, 0.0f, 0, 0, 0.5f, 0.0f, 0.8f, false},          // the spider, poisoned
    {38, 11, kTenth, 0.0f, 0, 18000, 0.0f, 0.0f, 1.0f, false},     // the plague fiend, blinded
    {39, 9, kTenth, 0.0f, 0, 0, 0.5f, kWearSeconds, 1.0f, false},  // the yeti, melted
    {40, 10, 0.0f, 500.0f, 0, 0, 0.25f, kWearSeconds, 1.0f, false}, // the wraith, shown
    {41, 7, 0.25f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, false},           // the lich, burned
    {42, 5, kTenth, 0.0f, 0, 0, 0.1f, kWearSeconds, 1.0f, false},  // the temple's, shaken
}};

/** The chimera, the lich and the temple's boss roar a second after rising; the rest three. */
bool quickToRoar(s32 boss) {
    return boss == 35 || boss == 41 || boss == 42;
}

} // namespace

const LegendWeakness* legendWeaknessOf(s32 kind) {
    const auto found = std::ranges::find(kWeaknesses, kind, &LegendWeakness::boss);
    return found != kWeaknesses.end() ? &*found : nullptr;
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
    const s32 roarWait = quickToRoar(m_weakness.boss) ? kShortRoarWait : kLongRoarWait;
    if (!m_roarDue && m_ticks >= roarWait) {
        m_roarDue = true;
    }
    if (m_roarDue && !m_roared && bossRoarDone) {
        m_roared = true;
        cues.push_back(LegendCue::Roared);
        m_wearLeft = m_weakness.curbLasts;
    }
    // Then its weakness runs its course, where it has one.
    if (m_roared && m_thrown) {
        if (m_weakness.curbLasts <= 0.0f) {
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

bool LegendShow::heldInHand(s32 kind) {
    return kind >= 34 && kind <= 39;
}

PlayerDeed LegendShow::gestureOf(s32 kind) {
    switch (kind) {
    case 36:
    case 37: return PlayerDeed::ShootLegend;
    case 34:
    case 35:
    case 38:
    case 39: return PlayerDeed::ThrowLegend;
    default: return PlayerDeed::HurlLegend;
    }
}

LegendShow::Flight LegendShow::flightOf(s32 kind) {
    switch (kind) {
    case 34:
    case 35:
    case 36:
    case 38: return Flight::Flies;
    case 37: return Flight::WithBearer;
    default: return Flight::AtBoss;
    }
}

std::string_view LegendShow::restingTreeOf(s32 kind) {
    return kind == 39 ? kBurstTree : kProjectileTree;
}

std::string_view LegendShow::burstTreeOf(s32 kind) {
    return kind == 39 ? kSecondBurstTree : kBurstTree;
}

f32 LegendShow::burstSecondsOf(s32 kind) {
    switch (kind) {
    case 41: return kLichBurstSeconds;
    case 37: return kSpiderBurstSeconds;
    default: return kBurstSeconds;
    }
}

Vec3 LegendShow::bossOffsetOf(s32 kind) {
    switch (kind) {
    case 39: return Vec3{-2.4375f, -2.3125f, 3.33203125f};
    case 40: return Vec3{0.0f, -2.3125f, 2.5625f};
    case 42: return Vec3{0.0f, 2.765625f, 2.125f};
    default: return Vec3{0.0f, 0.0f, 0.0f};
    }
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
