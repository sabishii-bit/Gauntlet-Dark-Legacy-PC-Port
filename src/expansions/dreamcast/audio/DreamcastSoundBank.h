#pragma once

#include <span>
#include <vector>

#include "engine/audio/SoundClip.h"

namespace gdl {

struct DreamcastBankSample {
    SoundClip clip;
    bool loop = false;
};

/** A Dreamcast AUDIO folder .BNK bank: a count followed by 44-byte sample descriptors.
 * This is not GameCube's DCS/VBK sequence format or N64's MusyX bank. */
std::vector<DreamcastBankSample> decodeDreamcastSoundBank(std::span<const u8> bytes);

} // namespace gdl
