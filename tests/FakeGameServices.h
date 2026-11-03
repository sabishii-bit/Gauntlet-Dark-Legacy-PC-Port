#pragma once

#include <functional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/characters/Party.h"
#include "game/config/ControlProfiles.h"
#include "game/online/PartyStore.h"
#include "game/scenes/ClassUnlocks.h"
#include "game/scenes/Haptics.h"

namespace gdl::test {
/** One rumble a controller was asked for. */
struct RumblePulse {
    s32 player = 0;
    s32 frames = 0;
    game::ControlFeedback feedback = game::ControlFeedback::Damage;
    bool operator==(const RumblePulse&) const = default;
};

/** The controllers' rumble as a test hears it: every pulse asked for, in turn, and how many
 * times the motors were stopped. */
class RecordingHaptics final : public game::Haptics {
public:
    using Pulse = RumblePulse;

    void vibrate(s32 player, s32 frames, game::ControlFeedback cue) override {
        pulses.push_back({player, frames, cue});
    }
    void stopRumble() override { ++stops; }

    std::vector<Pulse> pulses;
    s32 stops = 0;
};

/** The classes a test's play grants: each mask in turn, after `hook` has seen it. */
class RecordingUnlocks final : public game::ClassUnlocks {
public:
    void unlockClasses(u16 mask) override {
        if (hook) {
            hook(mask);
        }
        masks.push_back(mask);
    }

    std::function<void(u16)> hook; ///< runs first on every grant, for a test's own checks
    std::vector<u16> masks;
};

/** A test's save store: counts each checkpoint save and each explicit one, after `hook` has
 * seen the party; an explicit save answers `saveSucceeds`. */
class RecordingPartyStore final : public game::PartyStore {
public:
    void autoSave(std::span<game::PartyMember> party) override {
        if (hook) {
            hook(party);
        }
        ++autoSaves;
    }
    bool saveParty(std::span<game::PartyMember> party) override {
        if (hook) {
            hook(party);
        }
        ++saves;
        return saveSucceeds;
    }

    std::function<void(std::span<game::PartyMember>)> hook; ///< runs first on every save
    u32 autoSaves = 0;
    u32 saves = 0;
    bool saveSucceeds = true;
};
} // namespace gdl::test
