#pragma once

#include <optional>
#include <span>

#include "game/players/CharacterSave.h"

namespace gdl::game {
/** Public, selected-class state exchanged before loading. No disk slot, path,
 * other-class history, local unlock authority, or save-writing operation. */
struct CharacterProfile {
    std::string name;
    s32 character = 0;
    s32 color = 0;
    s32 gold = 0;
    s32 levelTotal = 0;
    bool autoAttack = true;
    ClassProgress progress;
    std::vector<s32> helpSeen;

    bool valid() const;
    static std::optional<CharacterProfile> capture(const CharacterSave& save);
    /** In-memory gameplay copy only; never use it to replace a local save. */
    CharacterSave gameplayCopy() const;
};

class CharacterProfilePacket {
public:
    static constexpr u32 kVersion = 1;
    static constexpr usize kBytes = 508;
    static std::optional<std::vector<u8>> encode(const CharacterProfile& profile);
    static std::optional<CharacterProfile> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
