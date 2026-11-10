#include <algorithm>
#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/menu/LegendsDreamcastDialogue.h"
#include "game/level/LevelWorld.h"
#include "game/menu/PauseMenu.h"

namespace {
using namespace gdl;
using namespace gdl::game;

class DreamcastPausePresentation final : public ImportedLevel {
public:
    LegendsDreamcastDialogue style;
    const DialoguePresentation* messagePresentation() const override { return &style; }
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*stopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*alpha*/) const override {}
};

void loadPauseWorld(LevelWorld& world, RenderDevice& device, const std::filesystem::path& root,
                    std::shared_ptr<ImportedLevel> imported) {
    CollisionTriangle floor;
    floor.normal = {0, 1, 0};
    floor.vertices = {{{-20, 0, -20}, {0, 0, 20}, {20, 0, -20}}};
    floor.object = 0;
    floor.objectFlags = 4;
    WorldCollision collision;
    collision.build({floor});
    LevelRef reference;
    reference.realm = "Legends";
    reference.realmId = -1;
    reference.name = "dreamcast:levelb1";
    reference.title = "Valley";
    auto layout = WorldLayout::fromDecoded({}, {}, {-20, 0, -20}, {20, 30, 20});
    const bool loaded = world.loadImported(device, root, reference, std::move(layout),
                                           std::move(collision), std::move(imported));
    REQUIRE(loaded);
}

bool drawnImage(const test::FakeRenderDevice& device, const Image& expected) {
    return std::ranges::any_of(device.draws, [&](const auto& draw) {
        const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
        return texture && texture->width() == expected.width &&
               texture->height() == expected.height && texture->pixels == expected.pixels &&
               !draw.vertices.empty() && std::ranges::any_of(draw.vertices, [](const auto& vertex) {
                   return vertex.color.a > 0;
               });
    });
}

void renderPause(PauseMenu& menu, test::FakeRenderDevice& device) {
    device.draws.clear();
    REQUIRE(device.beginFrame());
    menu.prepare(device);
    menu.render(device, makeScreenProjection(640, 448), 640, 448);
    device.endFrame();
}

TEST_CASE("Dreamcast imported pause retains the standard GDL menus and functional audio sliders",
          "[legends][dreamcast-pause][pause][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(native / "static/objects.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    auto imported = std::make_shared<DreamcastPausePresentation>();
    REQUIRE(imported->style.load(device, native));
    const auto nativeTextures = formats::decodeLegendsDreamcastTextures(
        readFile(native / "static/objects.rom"), readFile(native / "static/textures.rom"));
    const auto nativeImage = [&](std::string_view name) -> const Image& {
        const auto found =
            std::ranges::find(nativeTextures, name, &formats::LegendsMapTexture::name);
        REQUIRE(found != nativeTextures.end());
        return found->image;
    };
    TextureSet base;
    REQUIRE(base.load(root / "STATIC"));
    const auto scroll = base.find("SCROLL_A");
    const auto parchment = base.find("FONT32_PARCH");
    REQUIRE(scroll);
    REQUIRE(parchment);
    const auto checkStandardSkin = [&] {
        CHECK(drawnImage(device, base.image(*scroll)));
        CHECK(drawnImage(device, base.image(*parchment)));
        CHECK_FALSE(drawnImage(device, nativeImage("TEXTSCROLL")));
    };
    LevelWorld world;
    loadPauseWorld(world, device, root, imported);
    REQUIRE(world.imported() == imported.get());
    GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameServices services;
    services.config = &config;
    services.strings = &strings;
    services.unpackedRoot = root;
    test::FakeSettings settings;
    settings.target = &config;
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, services, party, 0, &settings, &world));
    menu.update(1, {});
    renderPause(menu, device);
    checkStandardSkin();

    MenuInput select;
    select.select = true;
    REQUIRE(menu.update(1.0 / 60, select) == PauseOutcome::Running); // Settings
    menu.update(1, {});
    renderPause(menu, device);
    checkStandardSkin();
    REQUIRE(menu.update(1.0 / 60, select) == PauseOutcome::Running); // Audio
    menu.update(1, {});
    REQUIRE(menu.musicAudible());
    renderPause(menu, device);
    checkStandardSkin();

    // A resident's native dialogue skin must not replace the normal settings
    // or their complete base-game track, fill, thumb and endcaps.
    TextureSet sliders;
    REQUIRE(sliders.load(root / "STATIC"));
    for (const auto* const name : AudioSlider::kTextures) {
        CAPTURE(name);
        const auto index = sliders.find(name);
        REQUIRE(index);
        CHECK(drawnImage(device, sliders.image(*index)));
    }
    const s32 initial = AudioSlider::value(config.audio.musicVolume);
    MenuInput left;
    left.left = true;
    REQUIRE(menu.update(1.0 / 60, left) == PauseOutcome::Running);
    menu.update(1.0 / 60, {}); // Releasing persists the preview.
    CHECK(settings.writes > 0);
    CHECK(AudioSlider::value(config.audio.musicVolume) == initial - 1);
    CHECK_FALSE(settings.audioPreviews.empty());
    CHECK(menu.party().size() == party.size());
}

TEST_CASE("Absent Dreamcast menu assets retain the working base pause menu",
          "[legends][dreamcast-pause][pause][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    auto imported = std::make_shared<DreamcastPausePresentation>();
    CHECK_FALSE(imported->style.load(device, test::scratchDirectory("dreamcast-menu-absent")));
    CHECK_FALSE(imported->style.menuText().ready());
    LevelWorld world;
    loadPauseWorld(world, device, root, imported);
    const GameConfig config;
    GameServices services;
    services.config = &config;
    services.unpackedRoot = root;
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, services, party, 0, nullptr, &world));
    menu.update(1, {});
    renderPause(menu, device);
    TextureSet base;
    REQUIRE(base.load(root / "STATIC"));
    const auto scroll = base.find("SCROLL_A");
    REQUIRE(scroll);
    CHECK(drawnImage(device, base.image(*scroll)));
    CHECK(menu.isOpen());
    CHECK_FALSE(device.draws.empty());
}

} // namespace
