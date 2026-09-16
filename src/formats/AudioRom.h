#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

struct AudioRomSound {
    std::string name;
    u32 id = 0;          ///< bank index << 16 | index within the bank
    f32 duration = 0.0f; ///< seconds; negative for sounds that loop until stopped
};

struct AudioRomBank {
    std::string file; ///< bank file stem as shipped, e.g. "common"
    std::string name; ///< upper-case bank name, e.g. "COMMON"
    u32 dataSize = 0;
    u32 soundCount = 0;
    u32 firstSound = 0; ///< index of the bank's first entry in AudioRom::sounds
};

/** The audio directory shipped beside the sound banks: bank records and every sound's name. */
struct AudioRom {
    std::vector<AudioRomBank> banks;
    std::vector<AudioRomSound> sounds;

    /** Parses the little-endian directory; throws FormatError. */
    static AudioRom parse(std::span<const u8> file);

    std::optional<u32> findBank(std::string_view name) const;
    std::optional<u32> findSound(std::string_view name) const;
};

} // namespace gdl::formats
