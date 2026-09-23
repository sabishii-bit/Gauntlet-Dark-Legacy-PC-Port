#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::formats {

struct AudioRomSound {
    std::string name;
    std::uint32_t id = 0;  ///< bank index << 16 | index within the bank
    float duration = 0.0f; ///< seconds; negative for sounds that loop until stopped
};

struct AudioRomBank {
    std::string file; ///< bank file stem as shipped, e.g. "common"
    std::string name; ///< upper-case bank name, e.g. "COMMON"
    std::uint32_t dataSize = 0;
    std::uint32_t soundCount = 0;
    std::uint32_t firstSound = 0; ///< index of the bank's first entry in AudioRom::sounds
};

/** The audio directory shipped beside the sound banks: bank records and every sound's name. */
struct AudioRom {
    std::vector<AudioRomBank> banks;
    std::vector<AudioRomSound> sounds;

    /** Parses the little-endian directory; throws FormatError. */
    static AudioRom parse(std::span<const std::uint8_t> file);

    std::optional<std::uint32_t> findBank(std::string_view name) const;
    std::optional<std::uint32_t> findSound(std::string_view name) const;
};

} // namespace gdl::formats
