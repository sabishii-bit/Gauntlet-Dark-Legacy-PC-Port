#pragma once

#include <filesystem>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"

#include "game/level/LevelCatalog.h"

namespace gdl::game {
/** The level-specific item archive, falling back to the realm's, and its ordered
 * texture lenders. Shared by level loading and native dependency validation. */
// NOLINTNEXTLINE(bugprone-exception-escape): a member's map move is not noexcept on every library
class LevelItemArchives {
public:
    bool load(const std::filesystem::path& root, const LevelRef& level);
    void clear();
    ItemArchive& primary() { return m_primary; }
    const ItemArchive& primary() const { return m_primary; }
    ItemArchive& realm() { return m_realm; }
    /** The level's archive then the realm's, the order the original searches. */
    // NOLINTNEXTLINE(modernize-return-braced-init-list): explicit constructor
    AssetScope scope() { return AssetScope(m_primary, m_realm.loaded() ? &m_realm : nullptr); }
    bool loaded() const { return m_primary.loaded(); }
    /** Borrowed for the lifetime of these archives; never a corpus-wide lookup. */
    std::vector<TextureSet*> textureLenders();

private:
    ItemArchive m_primary;
    ItemArchive m_realm;
};
} // namespace gdl::game
