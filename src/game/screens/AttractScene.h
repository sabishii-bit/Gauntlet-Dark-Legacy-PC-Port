#pragma once

#include <span>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/MenuInput.h"
#include "game/screens/GameContext.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** Traverses a level's camera-attract markers, beginning at its authored entry camera. */
class AttractCamera {
public:
    bool start(std::span<const WorldLocator> markers);
    void update(f32 seconds);
    const WorldCamera& camera() const { return m_camera; }
    bool finished() const { return m_finished; }

private:
    void selectNext();
    WorldCamera m_camera;
    WorldCamera m_from;
    std::vector<WorldLocator> m_remaining;
    WorldLocator m_target;
    f32 m_distance = 0.0f;
    f32 m_travelled = 0.0f;
    f32 m_hold = 0.0f;
    bool m_finished = true;
};

enum class AttractOutcome : u8 { Running, Finished, Title };

/** Read-only level demonstrations with their own world and audio, never a saved party. */
class AttractScene {
public:
    bool openNext(RenderDevice& device, const GameContext& context);
    void close();
    bool isOpen() const { return m_open; }
    AttractOutcome update(f64 seconds, const MenuInput& input);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    const AttractCamera& rail() const { return m_rail; }
    const LevelWorld& world() const { return m_world; }

private:
    LevelWorld m_world;
    LevelSoundscape m_audio;
    AttractCamera m_rail;
    GameContext m_context;
    BitmapFont m_font;
    TextureSet m_textures;
    TextPainter m_text;
    const Texture* m_glow = nullptr;
    Canvas m_canvas;
    usize m_nextRealm = 0;
    std::vector<usize> m_nextLevel;
    f64 m_elapsed = 0.0;
    bool m_open = false;
};
} // namespace gdl::game
