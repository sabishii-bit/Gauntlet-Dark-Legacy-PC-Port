#include "game/screens/PartyRecords.h"

#include <utility>

#include "engine/core/Types.h"

#include "game/players/Progression.h"
#include "game/players/TurboMeter.h"

namespace gdl::game {

void PartyRecords::award(std::span<PlayerRuntime> players, s32 player, s32 amount, bool kill,
                         const LevelInfo* level) {
    for (PlayerRuntime& runtime : players) {
        if (runtime.actor.player() != player || runtime.life != PlayerLife::Standing) {
            continue;
        }
        if (kill) {
            ++runtime.levelKills;
        }
        if (amount <= 0) {
            continue;
        }
        CharacterSave& save = runtime.actor.save();
        const f32 scale = level != nullptr
                              ? level->tuning.experienceScale(experienceLevel(save.experience()))
                              : 1.0f;
        const auto won = static_cast<s32>(static_cast<f32>(amount) * scale);
        save.progress().experience += won;
        const bool busy = runtime.figure != nullptr && runtime.figure->animator().turboing();
        if (kill && !busy) {
            runtime.turbo.add(TurboMeter::kPerExperience * static_cast<f32>(won));
        }
    }
}

std::vector<PartyMember> PartyRecords::members(std::span<const PlayerRuntime> players) {
    std::vector<PartyMember> members;
    members.reserve(players.size());
    // The fallen go on as they came into the level, less what it gave them (but what they
    // were taught stays taught).
    for (const PlayerRuntime& runtime : players) {
        if (runtime.departed) {
            continue; // gone from the game
        }
        const bool down = runtime.life != PlayerLife::Standing;
        PartyMember member{runtime.actor.player(), down ? runtime.entrySave : runtime.actor.save(),
                           runtime.slot, down};
        member.save.helpSeen = runtime.actor.save().helpSeen;
        member.helpHeard = runtime.helpHeard;
        members.push_back(std::move(member));
    }
    return members;
}

std::vector<PartyMember> PartyRecords::abandoned(std::span<const PlayerRuntime> players,
                                                 std::span<const PartyMember> party) {
    std::vector<PartyMember> members(party.begin(), party.end());
    for (PartyMember& member : members) {
        for (const PlayerRuntime& runtime : players) {
            if (runtime.actor.player() != member.player) {
                continue;
            }
            std::vector<s32> taught = std::move(member.save.helpSeen);
            member.save = runtime.entrySave;
            member.save.helpSeen = std::move(taught);
            member.fallen = false;
        }
    }
    return members;
}

std::vector<LevelResults> PartyRecords::results(std::span<const PlayerRuntime> players) {
    std::vector<LevelResults> results;
    for (const PlayerRuntime& runtime : players) {
        if (runtime.life == PlayerLife::Standing) {
            results.push_back(LevelResults::between(runtime.actor.player(), runtime.entrySave,
                                                    runtime.actor.save(), runtime.levelKills));
        }
    }
    return results;
}

} // namespace gdl::game
