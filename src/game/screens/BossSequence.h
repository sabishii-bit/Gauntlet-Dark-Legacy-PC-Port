#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>

#include "game/screens/BossVictoryPresentation.h"
#include "game/screens/LegendPresentation.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** Coordinates legend-item impacts and boss-death rewards, coins and the wizard visit.
 * Borrows level services, never the scene. Travel and draw order remain with the caller. */
class BossSequence {
public:
    struct Resources {
        RenderDevice& device;
        LevelWorld& world;
        ItemArchive& weapons;
        TextureSet& textures;
        EffectTrees& effects;
        LevelSoundscape& audio;
        const LevelCatalog* levels = nullptr;
    };

    BossSequence() = default;
    BossSequence(const BossSequence&) = delete;
    BossSequence& operator=(const BossSequence&) = delete;
    BossSequence(BossSequence&&) = delete;
    BossSequence& operator=(BossSequence&&) = delete;
    ~BossSequence() = default;

    void bind(Resources resources);
    /** Clear before releasing borrowed audio, archives or effects. Does not rewind coin RNG. */
    void clear();
    void showLegend(const LegendEvent& event, const Bosses& bosses,
                    std::span<const PlayerRuntime> players);
    void advanceLegend(float seconds, Bosses& bosses, std::span<PlayerRuntime> players);
    void fallen(const Vec3& where, const Bosses& bosses, std::span<PlayerRuntime> players);
    void spewCoins(const CritterSpew& spew, LevelOpponents& opponents,
                   std::span<PlayerRuntime> players);
    /** True only on the update that completes the visit; the caller chooses how to travel. */
    bool advanceVictory(std::int32_t ticks, float seconds, std::span<const PlayerRuntime> players,
                        const MessageTable& strings);
    const BossVictoryPresentation& victory() const { return m_victory; }
    const Texture* frozenTexture() const {
        return m_legend != nullptr ? m_legend->frozenTexture() : nullptr;
    }
    /** Player ids need not be contiguous; missing figures still supply fallback hold points. */
    static std::optional<LegendPresentation::Bearer> bearer(std::int32_t player, std::int32_t kind,
                                                            std::span<const PlayerRuntime> players);

private:
    std::optional<Resources> m_resources;
    std::mt19937 m_coinRandom{0xC01Eu};
    BossVictoryPresentation m_victory;
    std::unique_ptr<LegendPresentation> m_legend;
};
} // namespace gdl::game
