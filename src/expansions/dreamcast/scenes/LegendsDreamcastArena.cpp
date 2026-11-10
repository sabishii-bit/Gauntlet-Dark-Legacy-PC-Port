#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ObjectFlags.h"
#include "engine/core/Strings.h"
#include "engine/platform/Paths.h"

#include "expansions/dreamcast/level/LegendsDreamcastArenaRocks.h"
#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::bindDreamcastArenaRock(RenderDevice& device,
                                                 const formats::ItemInfoRecord& info,
                                                 const formats::ItemInstanceRecord& instance,
                                                 u32 /*nativeIndex*/) {
    auto admission = legendsDreamcastArenaRock(info, instance, m_arenaRocks.size());
    if (!admission.diagnostic.empty()) {
        // Each kind of safe rock the arena does not stand is reported once.
        admission.diagnostic += ": " + info.name;
        if (std::ranges::find(m_warnings, admission.diagnostic) == m_warnings.end()) {
            m_warnings.push_back(std::move(admission.diagnostic));
        }
    }
    if (!admission.rock) {
        // One past the roster stays scenery; one the arena cannot stand is never left as it.
        return !admission.ordinary;
    }
    const auto& rock = admission.rock;
    ArenaRockPresentation binding;
    binding.definition = rock->body;
    // SetItem8c0a8f80 multiplies native base HP by the placed segment count.
    // AddItemWobj8c0a9d80 changes the static model, not the item's position/body.
    binding.tier = rock->tier;
    binding.health = rock->tier * rock->body.tierHealth;
    binding.armor = rock->armor;
    binding.dormant = rock->dormant;
    AssetScope scope;
    for (auto& archive : m_archives)
        scope.add(*archive);
    ItemInstance placement;
    placement.position = instance.position;
    placement.rotation = instance.rotation;
    for (usize state = 0; state < binding.figures.size(); ++state) {
        const auto model = scope.findObject(std::format("{}{}", info.name, state));
        if (!model)
            continue; // Native AddItemWobj hides a genuinely absent damage model.
        const usize figure = m_fixtures.size();
        place(device, model->name, placement, 1, object_flags::kSorted, model->archive);
        if (figure == m_fixtures.size())
            continue;
        binding.figures[state] = figure;
        m_hiddenFixtures[figure] = true;
    }
    // AddItemWobj8c0a9d80 emits FX23 on the final tier change. InitFX8c0d33d0
    // binds that one-shot to this stage's native SAFEREXP tree.
    if (const auto burst = scope.findTree("SAFEREXP")) {
        const usize figure = m_fixtures.size();
        place(device, "SAFEREXP", placement, 1, object_flags::kSorted, burst->archive);
        if (figure != m_fixtures.size()) {
            binding.burstFigure = figure;
            m_hiddenFixtures[figure] = true;
        }
    }
    m_arenaRocks.push_back(binding);
    selectArenaRock(m_arenaRocks.size() - 1);
    return true;
}

void LegendsSceneAdapter::restoreDreamcastChimeraCover(RenderDevice& device,
                                                       const std::filesystem::path& directory,
                                                       ItemArchive* art) {
    if (m_platform != LegendsPlatform::Dreamcast ||
        toLowerAscii(paths::utf8(directory.filename())) != "levela5" || !m_arenaRocks.empty())
        return;
    // Compatibility restoration, not a recovered Dreamcast spawn script:
    // GC A5 items12..14 and N64 castle5 SHOOTW_1/2/3 retain these three blocks.
    // DC omits their placements/info but retains the same SAFEROCK1L1 mesh.
    // Use only that level's loaded DC artwork; never borrow another platform.
    if (art == nullptr || !art->models.find("SAFEROCK1L1"))
        return;
    constexpr std::array<Vec3, 3> positions{Vec3{-30.125f, .25f, -16.5f},
                                            Vec3{29.4609375f, .25f, -16.5f}, Vec3{0, 0, -6.5f}};
    // Convert the GC authored yaw to the DC matrix convention. These square
    // bodies are symmetric, but the texture orientation still follows the art.
    constexpr std::array<f32, 3> yaws{kHalfPi, -kHalfPi, 0};
    constexpr std::array<Vec3, 8> corners{
        Vec3{-4.5f, 0, -4.5f}, Vec3{4.5f, 0, -4.5f}, Vec3{4.5f, 0, 4.5f}, Vec3{-4.5f, 0, 4.5f},
        Vec3{-4.5f, 5, -4.5f}, Vec3{4.5f, 5, -4.5f}, Vec3{4.5f, 5, 4.5f}, Vec3{-4.5f, 5, 4.5f}};
    constexpr std::array<std::array<usize, 3>, 12> faces{{{0, 1, 2},
                                                          {0, 2, 3},
                                                          {4, 6, 5},
                                                          {4, 7, 6},
                                                          {0, 5, 1},
                                                          {0, 4, 5},
                                                          {1, 6, 2},
                                                          {1, 5, 6},
                                                          {2, 7, 3},
                                                          {2, 6, 7},
                                                          {3, 4, 0},
                                                          {3, 7, 4}}};
    for (usize index = 0; index < positions.size(); ++index) {
        ItemInstance placement;
        placement.position = positions[index];
        placement.rotation.y = yaws[index];
        const usize figure = m_fixtures.size();
        place(device, "SAFEROCK1L1", placement, 1, object_flags::kSorted, art);
        if (figure == m_fixtures.size())
            continue; // No invisible blocking volume when optional native art is absent.
        ArenaRockPresentation binding;
        binding.definition = {positions[index], yaws[index], 6.4f, 5, 100};
        binding.boxHalfExtents = {4.5f, 4.5f};
        binding.health = 100;
        binding.armor = 10;
        binding.tier = binding.initialTier = 1;
        binding.figures[1] = figure;
        const Mat4 transform =
            legendsDreamcastItemPlacement(placement.position, placement.rotation);
        for (const auto& face : faces) {
            CollisionTriangle triangle;
            for (usize vertex = 0; vertex < 3; ++vertex)
                triangle.vertices[vertex] = Vec3{transform * Vec4{corners[face[vertex]], 1}};
            triangle.normal =
                glm::normalize(glm::cross(triangle.vertices[1] - triangle.vertices[0],
                                          triangle.vertices[2] - triangle.vertices[0]));
            binding.surface.push_back(triangle);
        }
        m_arenaRocks.push_back(std::move(binding));
        selectArenaRock(m_arenaRocks.size() - 1);
    }
}

bool LegendsSceneAdapter::bindDreamcastEntranceRock(const formats::ItemInfoRecord& info,
                                                    const formats::ItemInstanceRecord& instance,
                                                    u32 index, usize figure) {
    if (figure >= m_fixtures.size() || !LegendsDreamcastBossEntrance::accepts(info, instance))
        return false;
    // The caller supplies the same AddItemSub placement as the visible figure.
    m_dreamcastBossEntrance.add(index, instance.position, instance.rotation);
    m_dreamcastBossEntranceFigures.push_back(figure);
    return true;
}

void LegendsSceneAdapter::cueDreamcastBossArena(Vec3 boss) {
    m_dreamcastBossEntrance.cue(boss);
}

void LegendsSceneAdapter::updateDreamcastBossArena(f32 seconds) {
    m_dreamcastBossEntrance.update(seconds);
    const auto pieces = m_dreamcastBossEntrance.pieces();
    for (usize i = 0; i < pieces.size(); ++i) {
        const auto figure = m_dreamcastBossEntranceFigures[i];
        const auto& motion = pieces[i].motion;
        m_hiddenFixtures[figure] = !motion.visible;
        m_fixtures[figure].placeAt(legendsDreamcastItemPlacement(motion.position, motion.rotation));
    }
}

} // namespace gdl::game
