#include "game/world/LevelItemArchives.h"

#include "engine/io/AssetLocator.h"

namespace gdl::game {
bool LevelItemArchives::load(const std::filesystem::path& root, const LevelRef& level) {
    clear();
    const auto hasAnimations = [](const std::filesystem::path& path) {
        const AssetLocator files(path);
        return files.find("anim.ps2").has_value() || files.find("animations.json").has_value();
    };
    const bool own = !level.ownItems.empty() && hasAnimations(root / level.ownItems) &&
                     m_primary.load(root / level.ownItems);
    if (!own && !m_primary.load(root / level.items)) {
        return false;
    }
    // Boss-specific art wins, while ordinary realm textures remain available.
    if (own && level.items != level.ownItems && hasAnimations(root / level.items)) {
        m_realm.load(root / level.items);
    }
    return true;
}

std::vector<TextureSet*> LevelItemArchives::textureLenders() {
    std::vector<TextureSet*> lenders;
    if (m_primary.loaded()) {
        lenders.push_back(&m_primary.textures);
    }
    if (m_realm.loaded()) {
        lenders.push_back(&m_realm.textures);
    }
    return lenders;
}

void LevelItemArchives::clear() {
    m_primary.clear();
    m_realm.clear();
}
} // namespace gdl::game
