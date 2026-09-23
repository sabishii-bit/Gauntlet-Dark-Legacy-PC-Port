#include "game/enemies/CombatantAssets.h"

#include <format>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
namespace gdl::game {
CombatantAssets::~CombatantAssets() {
    clear();
}
void CombatantAssets::clear() {
    textures.clear();
    body.clear();
    tree = nullptr;
    archive.clear();
}
bool CombatantAssets::load(RenderDevice& device, const std::filesystem::path& root,
                           const CombatantDefinition& family, char realm) {
    clear();
    definition = family;
    if (definition.name.empty() || !data.load(root / "critter" / (definition.name + ".json"))) {
        return false;
    }
    if (definition.kind == CombatantKind::Unknown || data.kind() != definition.kind) {
        log::warn("combatant {}: descriptor family {} does not match requested family {}",
                  definition.name, static_cast<s32>(data.kind()),
                  static_cast<s32>(definition.kind));
        return false;
    }
    const auto directory =
        definition.realmCostume
            ? root / "MONSTERS" / normalizeAssetName(data.folder()) / std::format("LEVEL{}", realm)
            : root / "MONSTERS" / definition.name;
    if (!archive.load(directory)) {
        return false;
    }
    const auto index = archive.trees.find(data.tree());
    if (!index.has_value()) {
        log::warn("combatant {}: no tree {}", definition.name, data.tree());
        clear();
        return false;
    }
    tree = &archive.trees.tree(*index);
    if (!body.bind(*tree, archive.models, archive.textures, device)) {
        clear();
        return false;
    }
    textures.bind(archive.trees.textureAnimations(), archive.textures, device);
    return true;
}
} // namespace gdl::game
