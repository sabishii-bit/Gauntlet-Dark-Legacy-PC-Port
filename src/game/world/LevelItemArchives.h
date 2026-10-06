#pragma once

#include <filesystem>
#include <vector>

#include "engine/assets/ItemArchive.h"

#include "game/world/LevelCatalog.h"

namespace gdl::game {
/** The level-specific item archive, falling back to the realm's, and its ordered
 * texture lenders. Shared by level loading and native dependency validation. */
class LevelItemArchives {
public:
    bool load(const std::filesystem::path& root, const LevelRef& level);
    void clear();
    ItemArchive& primary() { return m_primary; }
    ItemArchive& realm() { return m_realm; }
    bool loaded() const { return m_primary.loaded(); }
    /** Borrowed for the lifetime of these archives; never a corpus-wide lookup. */
    std::vector<TextureSet*> textureLenders();

private:
    ItemArchive m_primary;
    ItemArchive m_realm;
};
} // namespace gdl::game
