#include "engine/io/NativeStateDigest.h"

#include "game/party/PlayerRuntime.h"

namespace gdl {
void visitNativeState(NativeStateDigest& digest, const WallContact& value) {
    digest.fields<2>(value);
}
} // namespace gdl

namespace gdl::game {
// Each visitor decomposes the actual stored object. Changing its member count
// makes the structured binding fail to compile instead of silently losing state.
void visitNativeState(NativeStateDigest& digest, const PlayerSimulation& value) {
    digest.fields<46>(value);
}

void visitNativeState(NativeStateDigest& digest, const PlayerSimulation::AttackStep& value) {
    digest.fields<2>(value);
}

void visitNativeState(NativeStateDigest& digest, const PlayerSimulation::ShockGap& value) {
    digest.fields<2>(value);
}

void visitNativeState(NativeStateDigest& digest, const PlayerSimulation::Floor& value) {
    digest.fields<3>(value);
}

void visitNativeState(NativeStateDigest& digest, const CharacterSave& value) {
    digest.fields<10>(value);
}

void visitNativeState(NativeStateDigest& digest, const ClassProgress& value) {
    digest.fields<15>(value);
}

void visitNativeState(NativeStateDigest& digest, const LifetimeStats& value) {
    digest.fields<5>(value);
}

void visitNativeState(NativeStateDigest& digest, const Inventory& value) {
    digest.fields<4>(value);
}

void visitNativeState(NativeStateDigest& digest, const PowerupSlot& value) {
    digest.fields<5>(value);
}

void visitNativeState(NativeStateDigest& digest, const Relics& value) {
    digest.fields<7>(value);
}

void visitNativeState(NativeStateDigest& digest, const LevelRecord& value) {
    digest.fields<4>(value);
}

void visitNativeState(NativeStateDigest& digest, const LevelResults::Checkpoint& value) {
    // This linked-stage baseline is itself a stored native value, not a newly
    // constructed transport checkpoint or profile-normalized save projection.
    digest.fields<3>(value);
}

void visitNativeState(NativeStateDigest& digest, const ComboState& value) {
    digest.fields<9>(value);
}

void visitNativeState(NativeStateDigest& digest, const PlayerActor& value) {
    const auto& [player, save, position, yaw, speed, paceBonus, radius, height, followHeight,
                 attentionHeight, moving, stepAim, floorBase, floorObject, floorFlags, contacts] =
        value;
    // Every tick's update rewrites the step's aim before anything reads it: not body state.
    (void)stepAim;
    digest.values(player, save, position, yaw, speed, paceBonus, radius, height, followHeight,
                  attentionHeight, moving, floorBase, floorObject, floorFlags, contacts);
}

void visitNativeState(NativeStateDigest& digest, const Knockback& value) {
    const auto& [force, flags, damage, velocity, fastSeconds, frameSeconds, shove, shoved, pushed] =
        value;
    digest.values(force, flags, damage, velocity, fastSeconds, frameSeconds, shove, shoved, pushed);
}

void visitNativeState(NativeStateDigest& digest, const PlayerCapture& value) {
    const auto& [body, velocity, supportHeight, damage, flying, owner, boss, supportXZ,
                 restoreSupport] = value;
    digest.values(body, velocity, supportHeight, damage, flying, owner, boss, supportXZ,
                  restoreSupport);
}

void visitNativeState(NativeStateDigest& digest, const PlayerTransport& value) {
    const auto& [destination, timer, needsStepOff] = value;
    digest.values(destination, timer, needsStepOff);
}

void visitNativeState(NativeStateDigest& digest, const MikeyDecoy& value) {
    const auto& [position, frames, state, sparkles, generation] = value;
    digest.values(position, frames, state, sparkles, generation);
}

void visitNativeState(NativeStateDigest& digest, const TurboMeter& value) {
    const auto& [held, shown, flash, flashTicks] = value;
    digest.values(held, shown, flash, flashTicks);
}

void visitNativeState(NativeStateDigest& digest, const TurboMove& value) {
    const auto& [pending, all, owed, named, weaponHidden, volleysShot] = value;
    digest.values(pending, all, owed, named, weaponHidden, volleysShot);
}

void visitNativeState(NativeStateDigest& digest, const HeadGem& value) {
    const auto& [handOfDeath, healthVamp, shown] = value;
    digest.values(handOfDeath, healthVamp, shown);
}

void visitNativeState(NativeStateDigest& digest, const MeleeStreak& value) {
    const auto& [count, quiet, running] = value;
    digest.values(count, quiet, running);
}

void visitNativeState(NativeStateDigest& digest, const ExitWait& value) {
    const auto& [ticks] = value;
    digest.values(ticks);
}
} // namespace gdl::game
