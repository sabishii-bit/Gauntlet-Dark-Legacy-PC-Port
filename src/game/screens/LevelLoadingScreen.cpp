#include "game/screens/LevelLoadingScreen.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>

#include "engine/assets/WorldData.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr f32 kCanvasWidth = 512;
constexpr f32 kCanvasHeight = 384;
constexpr f32 kTileSize = 256;
constexpr std::array<Vec2, 4> kTilePositions{
    {{0, 0}, {kTileSize, 0}, {0, kTileSize}, {kTileSize, kTileSize}}};
constexpr f32 kDashSeconds = 0.5f;
constexpr usize kMaxDashes = 8;
} // namespace

bool LevelLoadingScreen::movieWanted(std::string_view movie, std::span<const PartyMember> party) {
    return !movie.empty() && std::ranges::any_of(party, [&](const PartyMember& member) {
        return !member.fallen &&
               std::ranges::find(member.save.moviesSeen, movie) == member.save.moviesSeen.end();
    });
}

void LevelLoadingScreen::rememberMovie(std::string_view movie, std::span<PartyMember> party) {
    if (movie.empty()) {
        return;
    }
    for (auto& member : party) {
        if (!member.fallen &&
            std::ranges::find(member.save.moviesSeen, movie) == member.save.moviesSeen.end()) {
            member.save.moviesSeen.emplace_back(movie);
        }
    }
}

void LevelLoadingScreen::open(const GameContext& context, const LevelRef& level) {
    close();
    if (level.isTower()) {
        return;
    }
    m_active = true;
    m_name = level.name;
    m_sounds = context.sounds;
    m_textures.load(context.unpackedRoot / "MAPS" / ("LEVEL" + level.name));
    WorldData data;
    if (data.load(context.unpackedRoot / level.worldDataFile())) {
        if (const auto* info = data.level(level.name)) {
            m_movie = info->movie;
            m_points = info->mapPoints;
            const auto* audio = data.audio(info->audioIndex);
            if (!level.name.empty() &&
                m_bank.load(context.unpackedRoot / "audio" / ("MAP_" + level.name.substr(0, 1)))) {
                play("S_MAP_" + level.name.substr(0, 1), kNoSound, SoundCategory::Music);
                const auto entering =
                    audio != nullptr ? play(data.soundName(audio->enterSound)) : kNoSound;
                play("S_" + level.name + "NAME", entering);
            }
        }
    }
}

void LevelLoadingScreen::close() {
    if (m_sounds != nullptr) {
        for (const auto handle : m_handles) {
            m_sounds->stop(handle);
        }
    }
    m_handles.clear();
    m_sounds = nullptr;
    m_bank = {};
    m_textures.releaseTextures();
    m_textures = {};
    m_points.clear();
    m_name.clear();
    m_movie.clear();
    m_seconds = 0;
    m_dashes = 0;
    m_active = false;
}

SoundHandle LevelLoadingScreen::play(std::string_view name, SoundHandle after,
                                     SoundCategory category) {
    const auto sound = m_bank.find(name);
    if (m_sounds == nullptr || !sound) {
        return kNoSound;
    }
    try {
        const auto handle =
            m_sounds->playAfter(after, m_bank.sequence(*sound), 224.0f / 255.0f, category);
        m_handles.push_back(handle);
        return handle;
    } catch (const std::exception& e) {
        log::warn("Loading sound {}: {}", name, e.what());
        return kNoSound;
    }
}

usize LevelLoadingScreen::dashCount() const {
    usize count = 0;
    for (usize i = 1; i < m_points.size() && i <= kMaxDashes; ++i) {
        if (m_points[i].x < 0 || m_seconds <= static_cast<f32>(i - 1) * kDashSeconds) {
            break;
        }
        ++count;
    }
    return count;
}

f32 LevelLoadingScreen::previewAlpha() const {
    const f32 routeSeconds = static_cast<f32>(m_dashes > 0 ? m_dashes - 1 : 0) * kDashSeconds;
    return std::clamp((m_seconds - routeSeconds - kMapSeconds) / kCrossfadeSeconds, 0.0f, 1.0f);
}

bool LevelLoadingScreen::update(f32 seconds) {
    if (!m_active) {
        return true;
    }
    m_seconds += std::max(seconds, 0.0f);
    const auto count = dashCount();
    while (m_dashes < count) {
        play("S_MAPDOT" + m_name.substr(0, 1));
        ++m_dashes;
    }
    const f32 routeSeconds = static_cast<f32>(m_dashes > 0 ? m_dashes - 1 : 0) * kDashSeconds;
    return m_seconds >= routeSeconds + kMapSeconds + kPreviewSeconds;
}

void LevelLoadingScreen::tile(Canvas& canvas, RenderDevice& device, std::string_view name,
                              const Vec2& position, f32 alpha, f32 scale) {
    if (const auto index = m_textures.find(name)) {
        const auto& entry = m_textures.entry(*index);
        canvas.draw(m_textures.texture(device, *index),
                    Rect{position.x * scale, position.y * scale,
                         static_cast<f32>(entry.width) * scale,
                         static_cast<f32>(entry.height) * scale},
                    Color::white().withAlpha(static_cast<u8>(255 * std::clamp(alpha, 0.0f, 1.0f))));
    }
}

void LevelLoadingScreen::draw(Canvas& canvas, RenderDevice& device, f32 width) {
    const f32 scale = width / kCanvasWidth;
    canvas.fill({0, 0, width, kCanvasHeight * scale}, Color::black());
    for (s32 i = 0; i < 4; ++i) {
        tile(canvas, device, std::format("MAP_{}_{:02}", m_name, i),
             kTilePositions[static_cast<usize>(i)], 1, scale);
    }
    for (usize i = 0; i < m_dashes; ++i) {
        tile(canvas, device, std::format("DASH_{}_{}", m_name, i + 1), m_points[i + 1], 1, scale);
    }
    if (!m_points.empty() && m_points.front().x >= 0) {
        const f32 routeSeconds = static_cast<f32>(m_dashes > 0 ? m_dashes - 1 : 0) * kDashSeconds;
        const auto frame = static_cast<s32>(std::max(0.0f, m_seconds - routeSeconds) * 60);
        const s32 phase = frame % 130;
        const s32 ramp = std::max(0, phase > 60 ? 120 - phase : phase);
        const f32 glow = static_cast<f32>(std::clamp((59 + ramp * 255) / 60, 4, 250)) / 255;
        tile(canvas, device, "MAP_" + m_name + "GLOW", m_points.front(), glow, scale);
    }
    for (s32 i = 0; i < 4; ++i) {
        tile(canvas, device, std::format("LDMAP_{}_{:02}", m_name, i),
             kTilePositions[static_cast<usize>(i)], previewAlpha(), scale);
    }
}

} // namespace gdl::game
