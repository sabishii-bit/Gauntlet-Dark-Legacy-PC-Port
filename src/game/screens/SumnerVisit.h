#pragma once

#include <cstdint>
#include <optional>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/ui/ModelSprite.h"

#include "game/config/GameConfig.h"
#include "game/menu/HintMenu.h"
#include "game/world/SumnerHints.h"

namespace gdl::game {

/** A visit to Sumner: greeting delay, localized hints and the player's scroll.
 * Borrows menu artwork and the text painter; clear before their owners release them.
 * The scene chooses visitors and applies sound/gesture cues, but no scene is retained. */
class SumnerVisit {
public:
    static constexpr float kGreetingSeconds = 2.0f;

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
    /** Ends the interaction and releases borrowed art, retaining hint progression. */
    void clear();

    /** True requests Sumner's welcome gesture. A completed delay opens the current
     * visitor's scroll. The delay keeps running while nobody occupies the spot. */
    bool visit(float seconds, std::optional<std::int32_t> visitor, bool sumnerReady,
               const TextPainter& text, const GameConfig* config, const StringTable* strings);
    /** The caller routes only owner()'s menu input. Returns sound/gesture cues. */
    HintMenuEvent update(RenderDevice& device, const MenuInput& input, std::int32_t ticks);
    /** Responds to Asked with a fresh party snapshot; no party scan is needed otherwise. */
    void answer(std::int32_t topic, const TextPainter& text, const StringTable* strings,
                const HintKnowledge& knowledge);
    void prepare(RenderDevice& device) { m_menu.prepare(device); }
    void draw(Canvas& canvas, const TextPainter& text) const { m_menu.draw(canvas, text); }

    bool active() const { return m_menu.active(); }
    std::int32_t owner() const { return m_owner; }
    const HintMenu& menu() const { return m_menu; }
    const SumnerHints& texts() const { return m_hints; }

private:
    void open(std::int32_t player, const TextPainter& text, const GameConfig* config,
              const StringTable* strings);

    SumnerHints m_hints;
    HintMenu m_menu;
    ModelSprite m_arrow; ///< menu art points here; this owner must not move
    std::int32_t m_owner = -1;
    float m_greetingLeft = -1.0f;
    bool m_hintsGiven = false;
};

} // namespace gdl::game
