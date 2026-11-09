#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expansions/shared/characters/LegendsProgress.h"
#include "formats/WorldFile.h"

namespace gdl::game {

struct LegendsDreamcastStage {
    std::string map;
    s32 world = -1;
    s32 stage = -1;
    s32 order = -1;
};

/** The Dreamcast's WRLD/LEVL roster and TowerInit admission policy. Its level
 * codes and progress order are independent of N64 map names and GDL realms. */
class LegendsDreamcastPortalPolicy {
public:
    void addWorld(std::span<const u8> worlddata);
    std::span<const LegendsDreamcastStage> stages() const { return m_stages; }
    const LegendsDreamcastStage* find(std::string_view map) const;
    bool allows(std::string_view map, const LegendsProgress& progress) const;

private:
    std::vector<LegendsDreamcastStage> m_stages;
};

struct LegendsDreamcastPortal {
    u32 nativeIndex = 0;
    std::string map;
    Vec3 position{0};
    f32 radius = 0;
    s32 minimumPlayers = 0;
};

/** Only ordinary authored exits are admitted. Secret/script destinations need
 * their own caller context rather than interpreting a parameter as a path. */
std::optional<LegendsDreamcastPortal>
dreamcastLegendsPortal(const formats::ItemInfoRecord& info,
                       const formats::ItemInstanceRecord& instance, u32 index,
                       const LegendsDreamcastPortalPolicy& policy);

bool touchesDreamcastPortal(const LegendsDreamcastPortal& portal, Vec3 feet, f32 radius,
                            usize partySize);

/** Temple, Underworld and paired Battlefield stages retain the run's tally
 * checkpoint across their intermediate exits without opening the shop. */
bool dreamcastContinuesRun(std::string_view source, std::string_view destination);

/** EXIT_PORTAL's five authored sequences, with durations in animation
 * sixtieths. Raised rings wait for every living player; ACTIVE3 must complete
 * before departure. Leaving the portal cancels departure credit. */
class LegendsDreamcastPortalMotion {
public:
    enum class Phase : u8 { Idle, Ready, Rising, Active, Closing };
    /** Native item timers round the authored duration to 30 Hz first, then
     * convert to the 60 Hz item clock. They do not ceil individual sixtieths. */
    static s32 sequenceTicks(s32 frames, s32 frameRate);
    explicit LegendsDreamcastPortalMotion(std::array<s32, 5> sequenceTicks = {});
    bool advance(f32 seconds, u8 occupants, u8 living, bool enabled);
    void reset();
    Phase phase() const { return m_phase; }
    u32 generation() const { return m_generation; }

private:
    void enter(Phase phase);
    bool tick(u8 occupants, u8 living);
    std::array<s32, 5> m_sequenceTicks{};
    Phase m_phase = Phase::Idle;
    s32 m_ticks = 0;
    u32 m_generation = 0;
    f64 m_remainder = 0;
    bool m_departed = false;
};

} // namespace gdl::game
