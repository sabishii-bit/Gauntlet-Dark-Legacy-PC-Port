#include "game/app/ImportedShop.h"

#include <algorithm>
#include <array>

#include "game/scenes/PlayScene.h"

namespace gdl::game {
bool commitImportedShop(PlayScene& scene, std::span<const PartyMember> before,
                        std::span<const PartyMember> after) {
    if (!scene.isOpen() || scene.world() == nullptr || !scene.world()->imported() ||
        before.empty() || before.size() > 4 || before.size() != after.size()) {
        return false;
    }
    const auto current = scene.party();
    if (current.size() != before.size()) {
        return false;
    }
    std::array<bool, 4> seen{};
    for (const auto& returned : after) {
        const s32 player = returned.player;
        if (player < 0 || player >= 4 || seen[static_cast<usize>(player)]) {
            return false;
        }
        seen[static_cast<usize>(player)] = true;
        const auto entry = std::ranges::find(before, player, &PartyMember::player);
        const auto live = std::ranges::find(current, player, &PartyMember::player);
        if (entry == before.end() || live == current.end() || entry->save != live->save ||
            entry->slot != live->slot || entry->fallen != live->fallen ||
            returned.fallen != entry->fallen || returned.slot != entry->slot ||
            returned.save.character != entry->save.character ||
            returned.save.name != entry->save.name || returned.save.color != entry->save.color) {
            return false;
        }
    }
    // No scene reset, actor placement or entry/results-checkpoint replacement.
    // Every seat was validated before the first save is changed.
    for (const auto& member : after) {
        *scene.liveSave(member.player) = member.save;
        scene.restat(member.player);
    }
    return true;
}
} // namespace gdl::game
