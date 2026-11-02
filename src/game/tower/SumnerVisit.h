#pragma once

#include <memory>
#include <optional>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/ui/ModelSprite.h"

#include "game/config/GameConfig.h"
#include "game/tower/HintMenu.h"
#include "game/tower/SumnerHints.h"

namespace gdl::game {

/** A visit to Sumner: greeting delay, localized hints and the player's scroll.
 * Borrows menu artwork and the text painter; clear before their owners release them.
 * The scene chooses visitors and applies sound/gesture cues, but no scene is retained. */
class SumnerVisit {
    struct Simulation {
        s32 owner = -1;
        f32 greetingLeft = -1.0f;
        bool hintsGiven = false;
    };

public:
    class LocalSnapshot {
        friend class SumnerVisit;
        Simulation m_state;
        const SumnerVisit* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
        HintMenu::LocalSnapshot m_menu;
        SumnerHints::LocalSnapshot m_hints;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static constexpr f32 kGreetingSeconds = 2.0f;

    SumnerVisit() = default;
    ~SumnerVisit() = default;
    SumnerVisit(const SumnerVisit&) = delete;
    SumnerVisit& operator=(const SumnerVisit&) = delete;
    SumnerVisit(SumnerVisit&&) = delete;
    SumnerVisit& operator=(SumnerVisit&&) = delete;

    /** Loads the text and borrowed art; returns the glow sheet also used by selectors. */
    const Texture* load(RenderDevice& device, TextureSet& textures, ItemArchive& powerups,
                        const std::filesystem::path& root, const StringTable* strings);
    /** Loads hint text independently of artwork (also usable with a text-only scroll). */
    bool loadTexts(const std::filesystem::path& file, const StringTable* strings = nullptr);
    /** Reuses resident-menu artwork without borrowing the base campaign's hint rules. */
    bool openTopics(s32 player, const TextPainter& text, const MenuScreen& screen,
                    HintMenuLabels labels);
    void closeTopics();
    /** `player` has left play: a scroll it holds goes at once, unanswered and unburnt. */
    void release(s32 player);
    /** Ends the interaction and releases borrowed art, retaining hint progression. */
    void clear();

    /** True requests Sumner's welcome gesture. A completed delay opens the current
     * visitor's scroll. The delay keeps running while nobody occupies the spot. */
    bool visit(f32 seconds, std::optional<s32> visitor, bool sumnerReady, const TextPainter& text,
               const GameConfig* config, const StringTable* strings);
    /** The caller routes only owner()'s menu input. Returns sound/gesture cues. */
    HintMenuEvent update(RenderDevice& device, const MenuInput& input, s32 ticks);
    /** Semantic topic input is checked again against the actual scroll owner. */
    HintMenuEvent updateNative(RenderDevice& device, s32 player, const MenuInput& input, s32 ticks,
                               u8 topic, u8 revision);
    u8 captureTopic(s32 player, const MenuInput& input) const;
    /** Responds to Asked with a fresh party snapshot; no party scan is needed otherwise. */
    void answer(s32 topic, const TextPainter& text, const StringTable* strings,
                const HintKnowledge& knowledge);
    void prepare(RenderDevice& device) { m_menu.prepare(device); }
    void draw(Canvas& canvas, const TextPainter& text) const { m_menu.draw(canvas, text); }

    bool active() const { return m_menu.active(); }
    bool nativeReady(const TextPainter& text) const {
        return m_hints.loaded() && text.ready() && m_menu.burnReady();
    }
    s32 owner() const { return m_sim.owner; }
    const HintMenu& menu() const { return m_menu; }
    const SumnerHints& texts() const { return m_hints; }

private:
    void open(s32 player, const TextPainter& text, const GameConfig* config,
              const StringTable* strings);

    SumnerHints m_hints;
    HintMenu m_menu;
    ModelSprite m_arrow; ///< menu art points here; this owner must not move
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
