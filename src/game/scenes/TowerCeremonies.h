#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/audio/LevelSoundscape.h"
#include "game/audio/SceneSoundEvents.h"
#include "game/audio/SpeechTiming.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PlayerRuntime.h"
#include "game/tower/LevelBeaten.h"
#include "game/tower/TowerPromotion.h"
#include "game/tower/TowerRelics.h"

namespace gdl::game {

class EffectTrees;
class ExitPortals;
class LevelWorld;

/**
 * The tower's ceremonies, run by the scene between ticks: the promotions the party has earned
 * (each character's new title spoken and its costume upgraded where it stands) and the relic
 * ceremonies (runestones and shards presented at the window, the routes they open revealed).
 * Owns the two presentations, their voices, and the costumes a promotion retires (kept alive
 * so saved effects can still borrow their class banks). The scene stays the sequencer: it
 * updates the world around a ceremony and keeps the gates that say when one may run.
 */
class TowerCeremonies {
public:
    /** What a ceremony borrows from the scene for one call. */
    struct Scene {
        RenderDevice& device;
        LevelWorld& world;
        std::span<PlayerRuntime> players;
        LevelSoundscape& audio;
        EffectTrees& effects;
        ItemArchive& weapons;
        const std::filesystem::path& unpackedRoot;
    };

    /** Begins what a tower arrival has earned: the promotions due, and the relic ceremonies,
     * already-presented pieces struck from everyone's pending bits, with Sumner's answer to the
     * level `beaten` on the way back when it calls for one. */
    void open(const Scene& scene, const MessageTable& hudStrings, const MessageTable& scrollStrings,
              ExitPortals& portals, const std::optional<LevelBeaten>& beaten);
    /** Forgets the ceremonies and their voices; the retained costumes stay until
     * releaseFigures(), after the soundscape that borrows their voices has closed. */
    void clearCeremonies();
    void releaseFigures();

    TowerPromotion& promotion() { return m_promotion; }
    const TowerPromotion& promotion() const { return m_promotion; }
    TowerRelics& relics() { return m_relics; }
    const TowerRelics& relics() const { return m_relics; }
    const LevelSoundscape::Speech& promotionVoice() const { return m_promotionVoice; }
    const LevelSoundscape::Speech& relicVoice() const { return m_relicVoice; }

    /** The ceremony steps the scene takes once its own preamble (figures, world, ambience)
     * has run and nobody is still materialising. */
    void advancePromotion(const Scene& scene, s32 ticks);
    void advanceRelics(const Scene& scene, ExitPortals& portals, s32 ticks, f32 seconds);
    /** Both voices' clocks, each frame. */
    void advanceSpeech(f32 seconds);
    /** The window routes' portals and glows, faded by what the ceremonies have revealed. */
    void revealRoutes(LevelWorld& world, ExitPortals& portals);

    /** Whether `figure` belongs to the member at `partyIndex`: its current costume, none, or
     * one retired here. */
    bool ownsFigure(std::span<const PlayerRuntime> players, usize partyIndex,
                    const PlayerFigure* figure) const;
    /** Stands the member at `partyIndex` in `figure`, retiring the costume it wore; null only
     * retires. */
    void activateFigure(std::span<PlayerRuntime> players, usize partyIndex, PlayerFigure* figure);
    /** Dresses the member at `partyIndex` in the costume its new level earns, loading it once
     * and replaying a discarded promotion from the cached birth values. */
    void upgradeFigure(const Scene& scene, usize partyIndex);

    /** The voices in the scene's snapshot, by their clocks and cues; their live handles stay.
     * The promotion and the relics are owners of the scene's roster in their own right. */
    void saveVoices(SpeechTiming& relicSpeech, SpeechTiming& promotionSpeech,
                    SoundCueToken& relicCue, SoundCueToken& promotionCue) const;
    void restoreVoices(const SpeechTiming& relicSpeech, const SpeechTiming& promotionSpeech,
                       const SoundCueToken& relicCue, const SoundCueToken& promotionCue);
    /** The retired costumes' borrowed native pointers, for the agreement digest's table. */
    bool registerRetainedFigures(NativeStateResources& resources) const;

private:
    /** A costume a promotion loaded, cached so a replay restores rather than reloads it. */
    struct PromotionFigure {
        usize partyIndex = 0;
        s32 character = 0;
        s32 color = 0;
        s32 level = 0;
        PlayerFigure* figure = nullptr;
        PlayerFigure::LocalSnapshot initial;
    };

    TowerPromotion m_promotion;
    TowerRelics m_relics;
    LevelSoundscape::Speech m_relicVoice;
    LevelSoundscape::Speech m_promotionVoice;
    std::vector<std::unique_ptr<PlayerFigure>>
        m_retainedFigures; ///< retain borrowed voice/effect clips until released
    std::vector<PromotionFigure> m_bindings;
};

} // namespace gdl::game
