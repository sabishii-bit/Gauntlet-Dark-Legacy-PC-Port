#pragma once

#include <filesystem>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"

namespace gdl {

/** One unpacked item archive: the models, textures and animation trees it holds together. */
struct ItemArchive {
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;

    /** Loads the three manifests under `directory`; false (with a warning) when any is
     * missing, leaving the archive empty. */
    bool load(const std::filesystem::path& directory);
    bool loaded() const { return models.loaded() && textures.loaded(); }
    /** Drops the GPU textures; call before the device goes away. */
    void release();
    /** Releases and forgets everything. */
    void clear();
};

} // namespace gdl
