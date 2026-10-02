#pragma once

#include "engine/assets/ParticleTemplate.h"

#include "formats/ParticleTemplate.h"

namespace gdl {

/** Convert the decoded retail record without a manifest round trip. Shared by level
 * and model animation files, which carry exactly the same emitter record. */
ParticleTemplate nativeParticle(const formats::ParticleTemplateRecord& source);

} // namespace gdl
