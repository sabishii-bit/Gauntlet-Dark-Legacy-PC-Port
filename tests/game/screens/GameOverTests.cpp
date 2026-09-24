#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/GameOver.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelSoundscape.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("game over uses the shipped caption and playable primary narrator cue",
          "[game][game-over][unpacked]") {
    const auto messagesPath = test::unpackedOrSkip("text/english.json");
    test::unpackedOrSkip("audio/VOICE1/sounds.json");
    MessageTable messages;
    REQUIRE(messages.load(messagesPath));
    const auto caption = messages.find(GameOver::kMessage);
    REQUIRE(caption.has_value());
    REQUIRE(*caption == 169);
    REQUIRE(messages.message(*caption).pages == std::vector<std::string>{"GAME OVER"});
    StringTable strings;
    strings.load(test::dataDirectory() / "text", "en");
    REQUIRE(strings.get(GameOver::kTextId) == messages.message(*caption).pages.front());
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape soundscape;
    soundscape.open(messagesPath.parent_path().parent_path(), &sounds, nullptr);
    const auto voice = soundscape.narrate(GameOver::kVoice, LevelSoundscape::Narrator::Primary);
    REQUIRE(voice != kNoSound);
    REQUIRE(sounds.isPlaying(voice));
    soundscape.close();
    REQUIRE_FALSE(sounds.isPlaying(voice));
}

TEST_CASE("game over waits for every participant's completed death", "[game][game-over]") {
    REQUIRE_FALSE(GameOver::ready({}));
    std::array<PlayerRuntime, 2> players;
    REQUIRE_FALSE(GameOver::ready(players));
    players[0].life = PlayerLife::InTower;
    REQUIRE_FALSE(GameOver::ready(players));
    players[1].life = PlayerLife::Dying;
    REQUIRE_FALSE(GameOver::ready(players));
    players[1].life = PlayerLife::InTower;
    REQUIRE(GameOver::ready(players));
}

TEST_CASE("game over announces once and reveals the caption on retail ticks", "[game][game-over]") {
    GameOver over;
    REQUIRE_FALSE(over.step(1000));
    REQUIRE_FALSE(over.finished());
    over.begin("GAME OVER");
    REQUIRE_FALSE(over.step(59));
    REQUIRE(over.letters() == 0);
    REQUIRE(over.step(1));
    REQUIRE(over.letters() == 0);
    REQUIRE_FALSE(over.step(7));
    REQUIRE(over.letters() == 0);
    REQUIRE_FALSE(over.step(1));
    REQUIRE(over.letters() == 1);
    REQUIRE_FALSE(over.step(64));
    REQUIRE(over.letters() == 9);
    over.begin("should not restart");
    REQUIRE_FALSE(over.step(107));
    REQUIRE_FALSE(over.finished());
    REQUIRE_FALSE(over.step(1));
    REQUIRE(over.finished());
    REQUIRE_FALSE(over.step(1000));
    over.clear();
    REQUIRE_FALSE(over.active());
    REQUIRE_FALSE(over.finished());
    REQUIRE(over.letters() == 0);
    over.begin("GAME OVER");
    REQUIRE_FALSE(over.step(-20));
    REQUIRE(over.step(1000));
    REQUIRE(over.finished());
}

TEST_CASE("game over tick batching preserves its duration and single announcement",
          "[game][game-over]") {
    for (const s32 stride : {1, 2, 4, 17}) {
        GameOver over;
        over.begin("GAME OVER");
        s32 ticks = 0;
        s32 announcements = 0;
        while (!over.finished()) {
            announcements += over.step(stride) ? 1 : 0;
            ticks += stride;
        }
        REQUIRE(announcements == 1);
        REQUIRE(ticks >= 240);
        REQUIRE(ticks < 240 + stride);
        REQUIRE(over.letters() == 9);
    }
}

TEST_CASE("game over reveals a fixed centred white double-size line", "[game][game-over]") {
    const BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    const test::FakeTexture sheet{32, 16};
    TextPainter text;
    text.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    Canvas canvas;
    GameOver over;
    over.begin("AB");
    over.step(68);
    canvas.begin(device, Mat4{1.0f});
    over.draw(canvas, text, 512);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 6);
    REQUIRE(test::minCorner(device.draws.front()).x == Approx(243));
    REQUIRE(test::minCorner(device.draws.front()).y == Approx(121));
    REQUIRE(device.draws.front().vertices.front().color == Color::white());
    device.draws.clear();
    over.step(8);
    canvas.begin(device, Mat4{1.0f});
    over.draw(canvas, text, 512);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 12);
    REQUIRE(test::minCorner(device.draws.front()).x == Approx(243));
}

} // namespace
