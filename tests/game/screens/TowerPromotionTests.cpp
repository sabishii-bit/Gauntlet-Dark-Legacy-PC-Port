#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/TowerPromotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void texts(MessageTable& table) {
    const auto path = test::scratchDirectory("tower-promotion-texts") / "text.json";
    writeTextFile(path, R"({"messages":[
        {"name":"NEWLEVEL","lines":["%s %s is now\na level %d %s!\n"]},
        {"name":"PLAYER_COLOR_LC","lines":["Yellow","Blue","Red","Green"]},
        {"name":"PLAYER_CLASS_LC","lines":["Warrior"]},
        {"name":"WAR_RANK","lines":["Fighter","Hero","Veteran","Champion","Master"]},
        {"name":"LEGEND","lines":["Legend"]}]})");
    REQUIRE(table.load(path));
}

void spawn(PlayerRuntime& player, s32 id, s32 level, s32 awarded) {
    CharacterSave save;
    save.progress().experience = levelExperience(level);
    save.progress().promotedLevel = awarded;
    player.actor.spawn(id, save, nullptr, Vec3{0}, 0);
}

TEST_CASE("tower promotions wait for their lead-in, award beats and completed speech",
          "[game][screens][promotion]") {
    std::array<PlayerRuntime, 2> party;
    spawn(party[0], 3, 30, 29);
    spawn(party[1], 1, 80, 79);
    TowerPromotion promotion;
    MessageTable strings;
    texts(strings);
    promotion.begin(party, strings);
    REQUIRE(promotion.active());
    REQUIRE(promotion.current()->player == 0); // party index, not controller id
    REQUIRE(promotion.current()->voice == "S_EXP30WAR");
    REQUIRE(promotion.current()->caption == "Yellow Warrior is now\na level 30 Hero!\n");
    REQUIRE_FALSE(promotion.update(119, false).voice);
    REQUIRE(promotion.shown() == 0);
    REQUIRE(promotion.update(1, false).voice);
    REQUIRE_FALSE(promotion.update(239, true).award);
    REQUIRE(promotion.update(1, true).award);
    REQUIRE_FALSE(promotion.update(29, true).gem);
    REQUIRE(promotion.update(1, true).gem);
    promotion.update(1000, true);
    REQUIRE(promotion.current()->player == 0); // voice still playing
    promotion.update(1, false);
    REQUIRE(promotion.current()->player == 1);
    REQUIRE(promotion.current()->voice == "S_EXP80WAR");
    REQUIRE(promotion.update(0, false).voice); // no second lead-in
    const auto cues = promotion.update(360, false);
    REQUIRE(cues.award);
    REQUIRE(cues.gem);
    REQUIRE(promotion.active()); // consumers must receive the awards before advancing
    promotion.update(1, false);
    REQUIRE_FALSE(promotion.active());
    REQUIRE(promotion.current() == nullptr);
    REQUIRE_FALSE(promotion.update(60, false).voice);
}

TEST_CASE("ordinary levels and awarded or fallen characters do not queue promotions",
          "[game][screens][promotion]") {
    std::array<PlayerRuntime, 4> party;
    spawn(party[0], 0, 19, 10);
    spawn(party[1], 1, 30, 30);
    spawn(party[2], 2, 80, 79);
    party[2].life = PlayerLife::InTower;
    spawn(party[3], 3, 99, 90);
    TowerPromotion promotion;
    MessageTable strings;
    texts(strings);
    promotion.begin(party, strings);
    REQUIRE(promotion.current()->player == 3);
    REQUIRE(promotion.current()->voice == "S_EXP99ALL");
    REQUIRE(promotion.current()->caption == "Yellow Warrior is now\na level 99 Legend!\n");
    party[3].actor.save().progress().promotedLevel = 99;
    promotion.begin(party, strings);
    REQUIRE_FALSE(promotion.active());
}

TEST_CASE("a catch-up promotion announces the attained level once, not each missed decade",
          "[game][screens][promotion]") {
    std::array<PlayerRuntime, 1> party;
    spawn(party[0], 0, 47, 9);
    TowerPromotion promotion;
    MessageTable strings;
    texts(strings);
    promotion.begin(party, strings);
    REQUIRE(promotion.current()->level == 47);
    REQUIRE(promotion.current()->voice == "S_EXP40WAR");
    REQUIRE(promotion.current()->caption == "Yellow Warrior is now\na level 47 Veteran!\n");
    promotion.update(1000, false);
    promotion.update(1, false);
    REQUIRE_FALSE(promotion.active());
}
TEST_CASE("promotion captions stay in the bottom bar while typing", "[game][promotion]") {
    MessageTable strings;
    texts(strings);
    std::array<PlayerRuntime, 1> party;
    spawn(party[0], 0, 30, 29);
    TowerPromotion promotion;
    promotion.begin(party, strings);
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 33; c < 127; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    const auto font = BitmapFont::fromGlyphs(32, 4, std::move(glyphs));
    test::FakeTexture sheet{128, 128};
    TextPainter text;
    text.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    Canvas canvas;
    promotion.update(122, false);
    canvas.begin(device, Mat4{1});
    promotion.drawCaption(canvas, text, 512, 384);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
    const auto first = test::minCorner(device.draws.front());
    REQUIRE(first.y >= 312);
    promotion.update(100, true);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    promotion.drawCaption(canvas, text, 512, 384);
    canvas.end();
    REQUIRE(test::minCorner(device.draws.front()) == first);
}

TEST_CASE("tower wizard exports include and render his animated body",
          "[game][promotion][unpacked]") {
    const auto root = test::unpackedOrSkip("ITEMS/LEVELL/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELL"));
    const auto wizard = items.trees.find("WIZARD");
    REQUIRE(wizard.has_value());
    const auto& tree = items.trees.tree(*wizard);
    const auto body = tree.findNode("OANIM");
    REQUIRE(body.has_value());
    INFO("Refresh tower exports with gdlunpack <asset-root> <unpacked-root> --only LEVELL");
    REQUIRE_FALSE(tree.nodes[*body].objectFrames.empty());
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    std::array<PlayerRuntime, 1> party;
    spawn(party[0], 0, 30, 29);
    MessageTable strings;
    texts(strings);
    TowerPromotion promotion;
    promotion.begin(party, strings);
    promotion.bind(device, items, layout, party);
    REQUIRE(promotion.camera().has_value());
    const Vec3 wizardPosition{promotion.wizardTransform()[3]};
    const Vec3 wizardFacing{promotion.wizardTransform()[2]};
    // The visible front faces the ceremony camera rather than showing its back.
    REQUIRE(glm::dot(wizardFacing, promotion.camera()->position - wizardPosition) > 0);
    promotion.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() >= 25);
    for (const auto& draw : device.draws) {
        REQUIRE(draw.blend() == BlendMode::Additive);
        REQUIRE_FALSE(draw.state.depthWrite);
    }
}
} // namespace
