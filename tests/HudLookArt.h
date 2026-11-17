#pragma once

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"
#include "formats/AnimationTree.h"
#include "game/hud/BossMeter.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HudView.h"

namespace gdl::test {

/** A boss's meter art: two strips, each a background and a fill `width` by `height`. */
inline TextureSet meterArt(u32 width, u32 height) {
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    for (const char* name : {"METER_BG1", "METER_FG1", "METER_BG2", "METER_FG2"}) {
        entries.push_back({.name = name, .width = width, .height = height, .file = {}});
        images.push_back(Image::filled(width, height, Color::white()));
    }
    return TextureSet::fromDecoded(std::move(entries), std::move(images));
}

/** A boss's meter of `pieces` strips with a background each and these insets, at `health` of
 * `maxHealth`. */
inline game::HealthMeterReading meterReading(s32 pieces, s32 leftInset, s32 rightInset, f32 health,
                                             f32 maxHealth) {
    game::HealthMeterReading reading;
    reading.definition.pieces = pieces;
    reading.definition.advance = game::BossMeter::kPieceWidth;
    reading.definition.leftInset = leftInset;
    reading.definition.rightInset = rightInset;
    reading.definition.shown = true;
    reading.definition.backed = true;
    reading.health = health;
    reading.maxHealth = maxHealth;
    return reading;
}

/** The meters of `readings` drawn from `art`, as the scene binds them, and told to `view`. */
inline void showMeters(game::HudView& view, game::BossMeters& meters,
                       std::span<const game::HealthMeterReading> readings, TextureSet& art) {
    meters.bind(readings, &art);
    view.bossMeters = &meters;
    view.bossCount = game::hudBossReadingsOf(readings, view.bossReadings);
}

/** The picture `name` of `art`, uploaded to `device` as a meter's draw uploads it. */
inline const Texture* meterPicture(TextureSet& art, FakeRenderDevice& device,
                                   std::string_view name) {
    const auto index = art.find(name);
    REQUIRE(index.has_value());
    return &art.texture(device, *index);
}

/** Dark Legacy's hourglass art as plain pictures: the glass, its sand and two frames of
 * falling grains, which SAND_ANIM cycles. */
inline ItemArchive darkLegacyHourglass() {
    std::vector<TextureSetEntry> entries{
        {.name = "TIMER", .width = 128, .height = 128, .file = {}},
        {.name = "TIMER_SAND", .width = 128, .height = 128, .file = {}},
        {.name = "SAND_ANIM00", .width = 8, .height = 32, .file = {}},
        {.name = "SAND_ANIM01", .width = 8, .height = 32, .file = {}}};
    std::vector<Image> images;
    images.reserve(entries.size());
    for (const TextureSetEntry& entry : entries) {
        images.push_back(Image::filled(entry.width, entry.height, Color::white()));
    }
    ItemArchive archive;
    archive.textures = TextureSet::fromDecoded(std::move(entries), std::move(images));
    formats::AnimationFile animations;
    animations.textureAnimations.push_back(
        {.name = "SAND_ANIM",
         .frameName = {},
         .texture = 2,
         .source = 2,
         .frames = 2,
         .rate = 1,
         .flag = static_cast<s16>(TextureAnimationInfo::kFreeRunning)});
    archive.trees = AnimationSet::fromDecoded(std::move(animations));
    return archive;
}

} // namespace gdl::test
