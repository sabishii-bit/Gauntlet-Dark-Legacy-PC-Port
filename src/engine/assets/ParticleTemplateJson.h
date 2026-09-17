#pragma once

#include <nlohmann/json_fwd.hpp>

#include "engine/assets/ParticleTemplate.h"

namespace gdl {

/** A template from its manifest entry; missing fields keep their defaults. */
ParticleTemplate readParticleTemplate(const nlohmann::json& entry);

} // namespace gdl
