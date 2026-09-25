#pragma once
#include <filesystem>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"

#include "game/enemies/CombatantDefinition.h"
#include "game/enemies/CritterData.h"

namespace gdl::game {
/** Stable shared assets for a fighter family. Release only after all borrowers finish. */
struct CombatantAssets {
    CombatantDefinition definition;
    CritterData data;
    std::vector<CritterData> children;
    ItemArchive archive;
    const TreeInfo* tree = nullptr;
    TreeModel body;
    TextureAnimator textures;
    CombatantAssets() = default;
    CombatantAssets(const CombatantAssets&) = delete;
    CombatantAssets& operator=(const CombatantAssets&) = delete;
    CombatantAssets(CombatantAssets&&) = delete;
    CombatantAssets& operator=(CombatantAssets&&) = delete;
    ~CombatantAssets();
    bool load(RenderDevice& device, const std::filesystem::path& root,
              const CombatantDefinition& family, char realm);
    void clear();
};
} // namespace gdl::game
