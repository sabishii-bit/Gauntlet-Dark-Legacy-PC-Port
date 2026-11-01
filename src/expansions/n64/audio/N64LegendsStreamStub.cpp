#include "expansions/n64/audio/N64LegendsStream.h"
#include "expansions/n64/audio/N64LegendsEffects.h"

namespace gdl {

/** Built without MusyX (GDL_LEGENDS_AUDIO off): native music/effects remain
 * unavailable, without adding a sequencer dependency to the base engine. */
struct N64LegendsStream::Impl {};

N64LegendsStream::N64LegendsStream() = default;
N64LegendsStream::~N64LegendsStream() = default;

bool N64LegendsStream::open(const std::filesystem::path& /*nativeAudioDirectory*/,
                            u16 /*songId*/) {
    return false;
}

bool N64LegendsStream::opened() const {
    return false;
}

AudioStreamDesc N64LegendsStream::desc() const {
    return {0, 0};
}

bool N64LegendsStream::read(std::vector<f32>& /*out*/, usize /*frames*/) {
    return false;
}

void N64LegendsStream::rewind() {}

struct N64LegendsEffects::Impl {};

N64LegendsEffects::N64LegendsEffects() = default;
N64LegendsEffects::~N64LegendsEffects() = default;

bool N64LegendsEffects::open(const std::filesystem::path& /*root*/) {
    return false;
}

bool N64LegendsEffects::opened() const {
    return false;
}

bool N64LegendsEffects::contains(u16 /*cue*/) const {
    return false;
}

std::span<const u16> N64LegendsEffects::cueIds() const {
    return {};
}

std::shared_ptr<StreamSource> N64LegendsEffects::create(u16 /*cue*/, f32 /*pitch*/,
                                                      f32 /*volume*/, f32 /*pan*/) {
    return {};
}

} // namespace gdl
