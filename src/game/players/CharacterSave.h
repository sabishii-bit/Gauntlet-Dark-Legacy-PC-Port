#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/ClassData.h"
#include "game/players/Progression.h"

namespace gdl::game {

inline constexpr usize kCharacterNameLength = 6;

/** One saved character: a name, its current class and costume, and progress with each class. */
struct CharacterSave {
    std::string name;
    s32 character = 0; ///< class index
    s32 color = 0;
    u16 classUnlock = 0; ///< one bit per unlockable class, from the ninth
    s32 gold = 0;
    s32 levelTotal = 0;
    std::vector<s32> helpSeen; ///< the help messages already shown to it, in order
    std::array<ClassProgress, kClassCount> classes{};

    const ClassProgress& progress() const { return classes[static_cast<usize>(character)]; }
    ClassProgress& progress() { return classes[static_cast<usize>(character)]; }
    s32 experience() const { return progress().experience; }

    /** The current class's health, full for a class never played. */
    s32 health() const { return progress().health > 0 ? progress().health : kStartingHealth; }

    std::string toJson() const;

    /** Throws FormatError on malformed text. */
    static CharacterSave fromJson(std::string_view text);
};

/** What a slot holds, without reading the whole save. */
struct SaveSlotInfo {
    bool exists = false;
    bool occupied = false; ///< a file exists, even when corrupt and not loadable
    std::string name;
    s32 character = 0;
    s32 color = 0;
};

/**
 * The numbered save files in one directory, standing in for the original's memory card: a
 * fixed number of slots, each empty or holding one character.
 */
class SaveSlots {
public:
    /** Points at `directory` (created when missing) and lists its slots; false when it
     * cannot be created. */
    bool open(const std::filesystem::path& directory, usize count);

    /** Re-reads the slot listing from disk. */
    void refresh();

    bool opened() const { return !m_directory.empty(); }
    usize count() const { return m_slots.size(); }
    const SaveSlotInfo& slot(usize index) const { return m_slots[index]; }
    bool anySaved() const;
    std::filesystem::path path(usize index) const;
    const std::filesystem::path& directory() const { return m_directory; }

    /** Reads a slot; false (with a warning) when it is empty or unreadable. */
    bool load(usize index, CharacterSave& out) const;

    /** Writes a slot; false (with a warning) when the file cannot be written. */
    bool write(usize index, const CharacterSave& save);

private:
    std::filesystem::path m_directory;
    std::vector<SaveSlotInfo> m_slots;
};

} // namespace gdl::game
