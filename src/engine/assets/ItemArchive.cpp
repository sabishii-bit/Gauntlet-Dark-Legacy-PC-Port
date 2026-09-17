#include "engine/assets/ItemArchive.h"

#include "engine/core/Log.h"

namespace gdl {

bool ItemArchive::load(const std::filesystem::path& directory) {
    clear();
    if (!models.load(directory) || !textures.load(directory) || !trees.load(directory)) {
        log::warn("Item archive under {} is not unpacked", directory.string());
        clear();
        return false;
    }
    return true;
}

void ItemArchive::release() {
    textures.releaseTextures();
}

void ItemArchive::clear() {
    release();
    models = ModelSet{};
    textures = TextureSet{};
    trees = AnimationSet{};
}

} // namespace gdl
