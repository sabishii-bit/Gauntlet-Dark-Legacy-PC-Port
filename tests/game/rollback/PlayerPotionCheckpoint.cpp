#include "game/rollback/PlayerPotionCheckpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

#include "engine/io/ByteReader.h"

#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
} // namespace

bool PlayerPotionState::valid() const {
    if (ticks > kMaxTicks || potions.size() > static_cast<usize>(Inventory::kMostPotions) ||
        std::ranges::any_of(potions, [](s32 kind) { return kind < 1 || kind > 4; }) ||
        (damageFlags & ~damage::kHeal) != 0 || bonuses[0] != 1 || !std::isfinite(powers[0]) ||
        powers[0] < PowerupEffects::kLeastMagicPower ||
        powers[0] > PowerupEffects::kMostMagicPower ||
        (textureFrame && (!std::isfinite(*textureFrame) || *textureFrame < 0 ||
                          *textureFrame > 1'000'000'000.0f))) {
        return false;
    }
    usize ownColours = 0;
    for (usize kind = 0; kind < powers.size(); ++kind) {
        if ((bonuses[kind] != 1 && bonuses[kind] != damage::kOwnColourBonus) ||
            !std::isfinite(powers[kind]) || powers[kind] != powers[0] * bonuses[kind]) {
            return false;
        }
        ownColours += bonuses[kind] != 1 ? 1U : 0U;
    }
    return ownColours <= 1;
}

std::optional<std::vector<u8>> PlayerPotionCheckpoint::encode(const PlayerPotionState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + 4 * state.potions.size());
    word(bytes, state.ticks);
    word(bytes, static_cast<u32>(state.potions.size()));
    word(bytes, state.damageFlags);
    for (const auto power : state.powers) {
        word(bytes, std::bit_cast<u32>(power));
    }
    for (const auto bonus : state.bonuses) {
        word(bytes, std::bit_cast<u32>(bonus));
    }
    word(bytes, state.textureFrame ? 1U : 0U);
    word(bytes, std::bit_cast<u32>(state.textureFrame.value_or(0)));
    for (const auto kind : state.potions) {
        word(bytes, static_cast<u32>(kind));
    }
    return bytes;
}

std::optional<PlayerPotionState> PlayerPotionCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    PlayerPotionState state;
    state.ticks = reader.readU32();
    const u32 count = reader.readU32();
    state.damageFlags = reader.readU32();
    if (count > static_cast<u32>(Inventory::kMostPotions) ||
        bytes.size() != kHeaderBytes + usize{4} * count) {
        return std::nullopt;
    }
    for (auto& power : state.powers) {
        power = std::bit_cast<f32>(reader.readU32());
    }
    for (auto& bonus : state.bonuses) {
        bonus = std::bit_cast<f32>(reader.readU32());
    }
    const u32 texturePresent = reader.readU32();
    const u32 texture = reader.readU32();
    if (texturePresent > 1 || (texturePresent == 0 && texture != 0)) {
        return std::nullopt;
    }
    if (texturePresent != 0) {
        state.textureFrame = std::bit_cast<f32>(texture);
    }
    state.potions.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        const u32 kind = reader.readU32();
        if (kind < 1 || kind > 4) {
            return std::nullopt;
        }
        state.potions.push_back(static_cast<s32>(kind));
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
} // namespace gdl::game
