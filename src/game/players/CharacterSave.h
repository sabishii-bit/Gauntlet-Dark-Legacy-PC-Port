#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "game/players/ClassData.h"
#include "game/players/Progression.h"

namespace gdl::game {

inline constexpr std::size_t kCharacterNameLength = 6;

/** One saved character: a name, its current class and costume, and progress with each class. */
struct CharacterSave {
    std::string name;
    int character = 0; ///< class index
    int color = 0;
    std::uint16_t classUnlock = 0; ///< one bit per unlockable class, from the ninth
    int gold = 0;
    int levelTotal = 0;
    std::vector<int> helpSeen; ///< the help messages already shown to it, in order
    std::array<ClassProgress, kClassCount> classes{};

    const ClassProgress& progress() const { return classes[static_cast<std::size_t>(character)]; }
    ClassProgress& progress() { return classes[static_cast<std::size_t>(character)]; }
    int experience() const { return progress().experience; }

    /** The current class's health, full for a class never played. */
    int health() const { return progress().health > 0 ? progress().health : kStartingHealth; }

    std::string toJson() const;

    /** Throws FormatError on malformed text. */
    static CharacterSave fromJson(std::string_view text);
};

/** What a slot holds, without reading the whole save. */
struct SaveSlotInfo {
    bool exists = false;
    std::string name;
    int character = 0;
    int color = 0;
};

/**
 * The numbered save files in one directory, standing in for the original's memory card: a
 * fixed number of slots, each empty or holding one character.
 */
class SaveSlots {
public:
    /** Points at `directory` (created when missing) and lists its slots; false when it
     * cannot be created. */
    bool open(const std::filesystem::path& directory, std::size_t count);

    /** Re-reads the slot listing from disk. */
    void refresh();

    bool opened() const { return !m_directory.empty(); }
    std::size_t count() const { return m_slots.size(); }
    const SaveSlotInfo& slot(std::size_t index) const { return m_slots[index]; }
    bool anySaved() const;
    std::filesystem::path path(std::size_t index) const;
    const std::filesystem::path& directory() const { return m_directory; }

    /** Reads a slot; false (with a warning) when it is empty or unreadable. */
    bool load(std::size_t index, CharacterSave& out) const;

    /** Writes a slot; false (with a warning) when the file cannot be written. */
    bool write(std::size_t index, const CharacterSave& save);

private:
    std::filesystem::path m_directory;
    std::vector<SaveSlotInfo> m_slots;
};

} // namespace gdl::game
