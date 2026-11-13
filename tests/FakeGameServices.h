#pragma once

#include <functional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/platform/DisplayMode.h"
#include "engine/render/RenderTypes.h"

#include "game/characters/Party.h"
#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/menu/LiveSettings.h"
#include "game/online/PartyStore.h"
#include "game/scenes/ClassUnlocks.h"
#include "game/scenes/Haptics.h"
#include "game/tower/TowerGateways.h"

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

/** The worlds past Sumner's gateways a test's installation offers: `offered` as it stands,
 * each ask counted. */
class FixedGateways final : public game::TowerGateways {
public:
    std::span<const game::GatewayWorld> worlds() override {
        ++asks;
        return offered;
    }

    std::vector<game::GatewayWorld> offered;
    u32 asks = 0;
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

/** A test's running settings: a save is refused while `fail` holds, else written back to
 * `target` and counted, after `hook` has seen it; each audio preview is kept in turn; the
 * Video trials answer through `video`, refused without one; the display and the renderer
 * are what the test sets. */
class FakeSettings final : public game::LiveSettings {
public:
    bool save(const game::GameConfig& next) override {
        if (hook) {
            hook(next);
        }
        if (fail) {
            return false;
        }
        if (target != nullptr) {
            *target = next;
        }
        ++writes;
        return true;
    }
    bool previewVideo(const game::GameConfig& trial) override { return video && video(trial); }
    void previewAudio(const game::AudioConfig& audio) override { audioPreviews.push_back(audio); }
    DisplayOptions display() const override { return displayOptions; }
    GraphicsApi renderer() const override { return drawingApi; }

    game::GameConfig* target = nullptr; ///< where a save is written back; nowhere without one
    std::function<void(const game::GameConfig&)> hook;  ///< runs first on every save
    std::function<bool(const game::GameConfig&)> video; ///< answers each Video trial
    std::vector<game::AudioConfig> audioPreviews;
    DisplayOptions displayOptions;
    GraphicsApi drawingApi = GraphicsApi::Vulkan;
    s32 writes = 0;
    bool fail = false;
};
} // namespace gdl::test
