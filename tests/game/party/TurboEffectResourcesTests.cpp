#include <array>
#include <filesystem>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/party/PlayerFigure.h"
#include "game/party/TurboEffectResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path resourceArchive(bool shared) {
    const auto root =
        test::scratchDirectory(shared ? "turbo-resources-weapons" : "turbo-resources-class");
    writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(root / "white.png", test::kTinyPng);
    writeTextFile(
        root / "textures.json",
        shared ? R"({"bitmaps":[{"name":"SPARK","file":"white.png","width":2,"height":2}]})"
               : R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json",
                  shared ? R"({"trees":[
        {"name":"SHARED","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":17,"frameRate":30}]},
        {"name":"SAME","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":21,"frameRate":30}]},
        {"name":"NULLFX","nodes":[{"name":"ROOT","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":33,"frameRate":30}]}]})"
                         : R"({"particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0],
                    "rate":[30,30,30,30],"flags":128,"flagMask":128}],
        "trees":[
        {"name":"SAME","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30}]},
        {"name":"PARTICLES","nodes":[{"name":"EMITTER","parent":-1,"position":[0,0,0],"particle":0}],
          "sequences":[{"name":"ACTIVE","frames":12,"frameRate":30}]}]})");
    test::convertModelFixture(root);
    return root;
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive effects;
    ItemArchive weapons;
    ClassStats stats;
    TurboEffectResources resources;
    Fixture() {
        REQUIRE(effects.load(resourceArchive(false)));
        REQUIRE(weapons.load(resourceArchive(true)));
        stats.moves.turboB = 0;
        stats.moves.turboC1 = 1;
        stats.moves.turboC2 = 2;
        stats.moveStrikes.resize(3);
        stats.moveEffects.resize(5);
        stats.moveStrikes[0].effect = 0;
        stats.moveStrikes[1].effect = 3;
        stats.moveStrikes[2].effect = 4;
        stats.moveEffects[0].tree = "SAME";
        stats.moveEffects[0].next = 1;
        stats.moveEffects[1].tree = "SHARED";
        stats.moveEffects[1].next = 2;
        stats.moveEffects[2].tree = "PARTICLES";
        stats.moveEffects[3].tree = "NULLFX";
        stats.moveEffects[4].tree = "SPARK";
        stats.moveEffects[4].flags = 0x01000000;
        stats.moveEffects[4].radius = 0.75f;
        stats.moveEffects[4].scale = 0.5f;
        stats.moveEffects[4].lifetime = 0.5f;
    }
    bool bind(std::span<TextureSet* const> lenders = {}) {
        return resources.bind(device, 0, stats, effects, weapons, lenders);
    }
};

TEST_CASE("turbo effect resources preserve native archives NULLFX and particle-only rows",
          "[turbo-effect-resources]") {
    Fixture f;
    const std::array<TextureSet*, 2> lenders{&f.weapons.textures, &f.effects.textures};
    REQUIRE(f.bind(lenders));
    REQUIRE(f.resources.stats() == &f.stats);
    REQUIRE(f.resources.character() == 0);
    const auto* strike = f.resources.strike(0);
    REQUIRE(strike);
    CHECK(strike->resources == std::vector<u32>{1, 2, 3});
    const auto* local = f.resources.resource(1);
    const auto* shared = f.resources.resource(2);
    const auto* authoredParticles = f.resources.resource(3);
    const auto* invisible = f.resources.resource(4);
    const auto* standalone = f.resources.resource(5);
    REQUIRE(local);
    REQUIRE(shared);
    REQUIRE(authoredParticles);
    REQUIRE(invisible);
    REQUIRE(standalone);
    CHECK(local->archive == &f.effects);
    CHECK(shared->archive == &f.weapons);
    CHECK(local->lenders == std::vector<TextureSet*>{lenders.begin(), lenders.end()});
    CHECK(local->tree->sequences[0].frames == 9);
    CHECK(local->model.bound());
    CHECK_FALSE(local->requiresParticles());
    CHECK_FALSE(authoredParticles->model.bound());
    CHECK(authoredParticles->particles.field().size() == 1);
    CHECK(authoredParticles->requiresParticles());
    CHECK(invisible->kind == TurboEffectResources::Kind::Invisible);
    CHECK(invisible->archive == &f.weapons);
    REQUIRE(invisible->tree);
    CHECK(f.resources.strike(1)->effectSeconds ==
          TurboEmission::effectSeconds(f.stats.moveEffects[3], invisible->tree));
    CHECK_FALSE(invisible->model.bound());
    CHECK(standalone->kind == TurboEffectResources::Kind::Particles);
    CHECK(standalone->archive == &f.weapons);
    CHECK(standalone->lenders.empty());
    CHECK(standalone->requiresParticles());
    REQUIRE(standalone->descriptor);
    CHECK(*standalone->descriptor == TurboEmission::particle(f.stats.moveEffects[4]).descriptor);
    CHECK(f.resources.strike(2)->effectSeconds == 0); // SPARK is a texture, not a timing tree.
    const auto uploads = f.device.texturesCreated;
    const auto particles = authoredParticles->particles.checkpoint();
    for (s32 repeat = 0; repeat < 20; ++repeat) {
        const auto prepared = f.resources.prepare(0, {0, {1, 2, 3}, {0, 0, 1}}, 10);
        REQUIRE(prepared);
        CHECK(prepared->effects.size() == 3);
        CHECK(prepared->effectSeconds == strike->effectSeconds);
        CHECK(f.resources.resource(3)->particles.checkpoint() == particles);
    }
    CHECK(f.device.texturesCreated == uploads);
    CHECK_FALSE(f.resources.prepare(0, {1}, 10));
    CHECK_FALSE(f.resources.prepare(0, {0, {}, {0, 0, 1}, true}, 10));
    CHECK_FALSE(f.resources.prepare(0, {0, {}, {0, 0, 1}, false, true}, 10));
    CHECK(f.resources.status(100) == TurboEffectResources::Status::NotTurbo);
    CHECK_FALSE(f.resources.strike(100));
    CHECK_FALSE(f.resources.resource(0));
}

TEST_CASE("turbo resource admission withholds complete unsupported or incomplete chains",
          "[turbo-effect-resources]") {
    Fixture f;
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    auto expected = TurboEffectResources::Status::UnsupportedStrike;
    switch (mutation) {
    case 0: f.stats.moveStrikes[0].type = MoveStrike::kFlies; break;
    case 1: f.stats.moveStrikes[0].speed = 1; break;
    case 2: f.stats.moveStrikes[0].hitEffect = 0; break;
    case 3: f.stats.moveStrikes[0].loopEffect = 1; break;
    case 4:
        f.stats.moveEffects[1].flags = 0x2000;
        expected = TurboEffectResources::Status::UnsupportedParent;
        break;
    case 5:
        f.stats.moveEffects[1].tree = "MISSING";
        expected = TurboEffectResources::Status::MissingResource;
        break;
    case 6:
        f.stats.moveEffects[2].next = 0;
        expected = TurboEffectResources::Status::InvalidChain;
        break;
    case 7:
        f.stats.moveEffects[2].next = 99;
        expected = TurboEffectResources::Status::InvalidChain;
        break;
    default: break;
    }
    REQUIRE(f.bind());
    CHECK(f.resources.status(0) == expected);
    CHECK_FALSE(f.resources.strike(0));
    CHECK_FALSE(f.resources.prepare(0, {}, 10));
    CHECK_FALSE(f.resources.resource(1));
    CHECK_FALSE(f.resources.resource(2));
    CHECK_FALSE(f.resources.resource(3));
    CHECK(f.resources.status(1) == TurboEffectResources::Status::Ready);
    CHECK(f.resources.status(2) == TurboEffectResources::Status::Ready);
}

TEST_CASE("turbo standalone sprites never guess resources from scene lenders",
          "[turbo-effect-resources]") {
    Fixture f;
    const auto root = test::scratchDirectory("turbo-resource-lender");
    writeFile(root / "image.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"name":"LENDERONLY","file":"image.png","width":2,"height":2}]})");
    TextureSet lender;
    REQUIRE(lender.load(root));
    f.stats.moveEffects[4].tree = "LENDERONLY";
    const std::array<TextureSet*, 1> lenders{&lender};
    REQUIRE(f.bind(lenders));
    CHECK(f.resources.status(2) == TurboEffectResources::Status::MissingResource);
    CHECK_FALSE(f.resources.resource(5));
    const auto* first = f.resources.strike(0);
    REQUIRE(first);
    const auto* stats = f.resources.stats();
    f.stats.moves.turboB = 10000;
    CHECK_FALSE(f.bind());
    CHECK(f.resources.strike(0) == first);
    CHECK(f.resources.stats() == stats);
    f.resources.clear();
    CHECK_FALSE(f.resources.stats());
    CHECK_FALSE(f.resources.strike(0));
}

TEST_CASE("native class turbo resource census identifies every supported and withheld B C row",
          "[turbo-effect-resources][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const s32 color = GENERATE(0, 1, 2, 3);
    CAPTURE(character, color);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const auto* stats = classes.stats(character);
    REQUIRE(stats);
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = character;
    save.color = color;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    auto* effects = figure->effects();
    REQUIRE(effects);
    TurboEffectResources resources;
    const std::array<TextureSet*, 1> lenders{&weapons.textures};
    REQUIRE(resources.bind(device, character, *stats, *figure, weapons, lenders));
    std::set<s32> seen;
    for (const s32 first : {stats->moves.turboB, stats->moves.turboC1, stats->moves.turboC2}) {
        for (const s32 row : stats->strikesOf(first)) {
            if (!seen.insert(row).second) {
                continue;
            }
            const auto status = resources.status(row);
            CAPTURE(row, static_cast<u32>(status));
            REQUIRE(status != TurboEffectResources::Status::NotTurbo);
            if (status != TurboEffectResources::Status::Ready) {
                const auto& definition = stats->moveStrikes[static_cast<usize>(row)];
                if (status == TurboEffectResources::Status::UnsupportedStrike) {
                    CHECK((definition.type == MoveStrike::kFlies || definition.speed != 0 ||
                           definition.hitEffect >= 0 || definition.loopEffect >= 0));
                } else if (status == TurboEffectResources::Status::UnsupportedParent) {
                    bool unsupported = false;
                    std::set<s32> chain;
                    for (s32 index = definition.effect;
                         index >= 0 && static_cast<usize>(index) < stats->moveEffects.size() &&
                         chain.insert(index).second;
                         index = stats->moveEffects[static_cast<usize>(index)].next) {
                        unsupported = unsupported ||
                                      (stats->moveEffects[static_cast<usize>(index)].flags &
                                       (0x10U | 0x40U | 0x80U | 0x800U | 0x2000U | 0x40000U)) != 0;
                    }
                    CHECK(unsupported);
                } else {
                    // Native tables should neither contain cyclic chains nor rely on
                    // missing resources for this conservative stationary subset.
                    CHECK(status == TurboEffectResources::Status::UnsupportedStrike);
                }
                CHECK_FALSE(resources.strike(row));
                CHECK_FALSE(resources.prepare(row, {character}, 20));
                continue;
            }
            const auto* bound = resources.strike(row);
            REQUIRE(bound);
            const auto prepared = resources.prepare(row, {character, {1, 2, 3}, {0, 0, 1}}, 20);
            REQUIRE(prepared);
            REQUIRE(prepared->effects.size() == bound->resources.size());
            const auto uploads = device.texturesCreated;
            for (usize ordinal = 0; ordinal < prepared->effects.size(); ++ordinal) {
                const auto& effect = prepared->effects[ordinal];
                const auto* resource = resources.resource(bound->resources[ordinal]);
                REQUIRE(resource);
                CHECK(resource->row == effect.row);
                if (effect.particle) {
                    CHECK(resource->kind == TurboEffectResources::Kind::Particles);
                    REQUIRE(resource->descriptor);
                    CHECK(*resource->descriptor == effect.particle->descriptor);
                    REQUIRE(resource->texture);
                } else if (effect.authored.tree.empty() || effect.authored.tree == "NULLFX") {
                    CHECK(resource->kind == TurboEffectResources::Kind::Invisible);
                } else {
                    CHECK(resource->kind == TurboEffectResources::Kind::Tree);
                    auto* expected = effects->trees.find(effect.authored.tree) ? effects : &weapons;
                    CHECK(resource->archive == expected);
                    REQUIRE(resource->tree);
                    CHECK(resource->tree->name == effect.authored.tree);
                    CHECK((resource->model.bound() || resource->particles.field().size() > 0));
                    CHECK(resource->particles.checkpoint().valid());
                }
            }
            CHECK(device.texturesCreated == uploads);
        }
    }
    CHECK_FALSE(seen.empty());
}

TEST_CASE("external exact class poses preserve native turbo root and named costume attachments",
          "[turbo-effect-resources][turbo-effect-attachments][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const auto deed = GENERATE(PlayerDeed::TurboStrong, PlayerDeed::TurboFull);
    CAPTURE(character, static_cast<u32>(deed));
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = character;
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->actionTree());
    std::vector<std::string> suffixes{"HEAD", "L_HAND", "R_HAND", "MISSING"};
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    REQUIRE(classes.stats(character));
    for (const auto& effect : classes.stats(character)->moveEffects) {
        if (effect.particle() && !effect.sound.empty()) {
            suffixes.push_back(effect.sound);
        }
    }
    bool sawTurbo = false;
    bool sawAttachment = false;
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(tick);
        figure->animate(0, 1, 1.0f / 60, tick == 0 ? deed : PlayerDeed::None);
        sawTurbo = sawTurbo || PlayerAnimator::rollbackTurboAction(figure->animator().action());
        const auto before = figure->animator().checkpoint();
        const TreePose exact = figure->animator().pose();
        const auto body =
            glm::translate(glm::rotate(Mat4{1}, 0.61f, Vec3{0, 1, 0}), Vec3{1.3f, 2.1f, 3.7f});
        CHECK(figure->rootAttachment(body, exact) == figure->rootAttachment(body));
        for (const auto& suffix : suffixes) {
            const auto native = figure->attachment(body, suffix);
            CHECK(figure->attachment(body, suffix, exact) == native);
            sawAttachment = sawAttachment || native.has_value();
        }
        CHECK(figure->animator().checkpoint() == before);
    }
    CHECK(sawTurbo);
    CHECK(sawAttachment);
}
} // namespace
