#include <array>
#include <cmath>
#include <filesystem>
#include <set>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/assets/LegendsN64Archive.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "formats/AnimationTree.h"
#include "formats/CritterWad.h"
#include "game/bosses/Bosses.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantProjectiles.h"
#include "game/effects/CritterCueEmission.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Native boss assets bind six original skeletons and every authored move",
          "[legends][native-boss-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native banks not installed");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(root / "_program/game.bin");
    test::FakeRenderDevice device;
    const std::array pressureRows{std::pair{34, 1000.0f}, std::pair{35, 2000.0f},
                                  std::pair{38, 1500.0f}, std::pair{39, 2500.0f},
                                  std::pair{42, 3000.0f}, std::pair{43, 5000.0f}};
    for (const auto& [kind, pressure] : pressureRows) {
        DYNAMIC_SECTION("boss " << kind) {
            std::vector<std::string> warnings;
            auto stock = loadLegendsBossAssets(device, root, gc, kind, program, {}, warnings);
            REQUIRE(stock);
            REQUIRE(stock->tree);
            CHECK(stock->body.bound());
            CHECK(stock->cooldownStartsWithMove);
            CHECK(stock->projectileLifetime == 5);
            CHECK(stock->wakeDelay == 4);
            CHECK(stock->data.rules().pressureReferenceDamage == pressure);
            CHECK(stock->data.rules().projectileTargetHeight == 4);
            CHECK(stock->data.rules().restoreCaptureSupport);
            CHECK(stock->tree->name == stock->data.tree());
            CHECK(stock->archive.models.loaded());
            CHECK(stock->archive.textures.loaded());
            if (kind == 34 || kind == 35 || kind == 43)
                REQUIRE(stock->archive.trees.find("FBALL_EXP"));
            const auto validate = [&](const CritterData& data) {
                for (const auto& move : data.moves()) {
                    CAPTURE(data.name(), move.name, move.anim, move.colnode);
                    const auto sequence = stock->tree->findSequence(move.anim);
                    REQUIRE(sequence);
                    if (!move.colnode.empty())
                        CHECK(stock->tree->findNode(move.colnode, kCombatantNodeNameLength));
                    TreePose pose;
                    const auto& info = stock->tree->sequences[*sequence];
                    for (const f32 fraction : {0.0f, 0.5f, 1.0f}) {
                        pose.evaluate(*stock->tree, *sequence,
                                      static_cast<f32>(std::max(info.frames - 1, 0)) * fraction);
                        for (const auto& matrix : pose.matrices()) {
                            for (usize col = 0; col < 4; ++col)
                                for (usize row = 0; row < 4; ++row)
                                    REQUIRE(std::isfinite(
                                        matrix[static_cast<int>(col)][static_cast<int>(row)]));
                        }
                    }
                }
            };
            validate(stock->data);
            for (const auto& child : stock->children) {
                CHECK(child.rules().pressureReferenceDamage == 1000);
                validate(child);
            }
            for (const auto& cue : stock->data.sounds()) {
                if (cue.tree == "ABALL_EXP" || cue.tree == "EBALL_EXP") {
                    REQUIRE(stock->archive.trees.find(cue.tree));
                    CHECK(cue.scale == Catch::Approx(0.72f));
                    CHECK(cue.impactRadius == Catch::Approx(3.6f));
                    CHECK(cue.impactPhysicalFraction ==
                          Catch::Approx(cue.tree == "ABALL_EXP" ? 0.1f : 0.2f));
                    CHECK(cue.impactElement == (cue.tree == "ABALL_EXP" ? 4U : 2U));
                } else {
                    CHECK(cue.scale == 1);
                }
            }
            if (kind == 35)
                CHECK(stock->children.size() == 3);
            if (kind == 34) {
                const auto claw = stock->tree->findSequence("CLWL");
                REQUIRE(claw);
                CHECK(stock->tree->sequences[*claw].frames == 75);
                CHECK(stock->tree->sequences[*claw].frameRate == 15);
                CHECK(stock->data.moves()[5].cooldown == 0.5f);
            }
            if (kind == 42) {
                for (const auto& [name, frame] :
                     {std::pair{"POUNDL", s32{10}}, std::pair{"POUNDR", s32{9}}}) {
                    const auto pound = stock->data.moveNamed(name);
                    REQUIRE(pound);
                    CHECK(stock->data.moves()[*pound].frameStart == frame);
                    const auto sequence =
                        stock->tree->findSequence(stock->data.moves()[*pound].anim);
                    REQUIRE(sequence);
                    CHECK(stock->tree->sequences[*sequence].frames == 27);
                    CHECK(stock->tree->sequences[*sequence].frameRate == 30);
                }
                const auto roar = stock->data.moveNamed("ROAR");
                REQUIRE(roar);
                const auto& move = stock->data.moves()[*roar];
                CHECK(move.soundFrame == 20);
                const auto* cue = stock->data.sound(move.sound);
                REQUIRE(cue);
                CHECK(cue->tree == "BOSSROARFX");
                CHECK(cue->soundFormat == "S_SKORN1PROAR");
            }
        }
    }
}

TEST_CASE("Native boss projectile banks render authored attacks and deliver their damage",
          "[legends][native-boss-projectiles][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native banks not installed");
    const auto program = readFile(root / "_program/game.bin");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    test::FakeRenderDevice device;
    for (const auto kind : {34, 35, 38, 39, 42, 43}) {
        DYNAMIC_SECTION("boss " << kind) {
            std::set<std::string> scaledImpacts;
            std::vector<std::string> warnings;
            auto stock = loadLegendsBossAssets(device, root, gc, kind, program, {}, warnings);
            REQUIRE(stock);
            usize tested = 0;
            for (usize index = 0; index < stock->data.damages().size(); ++index) {
                const auto& damage = stock->data.damages()[index];
                if (damage.type != AttackDefinition::kProjectile || damage.speed <= 0 ||
                    damage.damage <= 0)
                    continue;
                CAPTURE(kind, index);
                const auto* cue = stock->data.sound(damage.sound);
                REQUIRE(cue);
                REQUIRE(stock->archive.trees.find(cue->tree));
                EffectTrees effects;
                CombatantProjectiles flights;
                CombatShot shot;
                shot.data = &stock->data;
                shot.damageIndex = static_cast<s32>(index);
                shot.origin = {0, 100, 0};
                shot.target = Vec3{0, 100, 30};
                shot.birthLife = stock->projectileLifetime;
                flights.launch(shot, stock->archive, device, effects, {});
                REQUIRE(flights.count() == 1);
                REQUIRE(effects.count() == 1);
                CHECK(effects.effect(0).secondsLeft == Catch::Approx(5));
                // Let the genuine native birth pose advance before inspecting its
                // submitted geometry; some first frames are deliberately empty.
                for (int tick = 0; tick < 4; ++tick) {
                    effects.update(1.0f / 120);
                    flights.update(1.0f / 120, nullptr, {}, device, effects, {});
                }
                device.draws.clear();
                effects.draw(device, Mat4{1}, WorldLighting{});
                REQUIRE_FALSE(device.draws.empty());
                bool textured = false;
                for (const auto& draw : device.draws)
                    textured |= !draw.vertices.empty() && draw.texture != &device.whiteTexture();
                CHECK(textured);
                // Place a victim at the actual flight point, not a guessed yaw or
                // borrowed GameCube attachment. Native flags and damage remain intact.
                const Vec3 feet = effects.effect(0).position - Vec3{0, 3, 0};
                const std::array players{
                    EnemyView{.player = 2, .position = feet, .radius = 1, .height = 6}};
                effects.update(1.0f / 120);
                flights.update(1.0f / 120, nullptr, players, device, effects, {});
                const auto hits = flights.takeHits();
                REQUIRE_FALSE(hits.empty());
                CHECK(hits.front().player == 2);
                CHECK(hits.front().damage == Catch::Approx(damage.damage));
                CHECK(hits.front().flags == damage.flags);
                if (const auto* impact = stock->data.sound(damage.hitSound);
                    impact && (impact->tree == "ABALL_EXP" || impact->tree == "EBALL_EXP") &&
                    ((damage.flags & 0x100000U) == 0 || (damage.flags & 0x200000U) != 0)) {
                    bool found = false;
                    for (usize effect = 0; effect < effects.count(); ++effect) {
                        const auto& shown = effects.effect(effect);
                        if (shown.name != impact->tree)
                            continue;
                        REQUIRE(shown.tree);
                        CHECK(shown.tree->name == impact->tree);
                        CHECK(shown.archive == &stock->archive);
                        CHECK(shown.scale == Catch::Approx(0.72f));
                        scaledImpacts.insert(shown.name);
                        found = true;
                    }
                    CHECK(found);
                }
                flights.clear(effects);
                ++tested;
            }
            CHECK(tested > 0);
            if (kind == 35 || kind == 38 || kind == 42)
                CHECK(scaledImpacts.contains("ABALL_EXP"));
            if (kind == 35 || kind == 42)
                CHECK(scaledImpacts.contains("EBALL_EXP"));
        }
    }
}

TEST_CASE("Imported Skorne roar trims only the pre-roll absent from its GDL counterpart",
          "[legends][native-skorn-render][native-roar-phase][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native banks not installed");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(root / "_program/game.bin");
    test::FakeRenderDevice device;
    for (const auto kind : {42, 43}) {
        CAPTURE(kind);
        const auto gcName = kind == 42 ? "SKORNE1" : "SKORNE2";
        const auto nativeName = kind == 42 ? "boss1" : "boss2";
        const auto counterpart =
            formats::AnimationFile::parse(readFile(gc / "MONSTERS" / gcName / "ANIM.PS2"));
        const auto gcTreeIndex = counterpart.find("BOSSROARFX");
        REQUIRE(gcTreeIndex);
        const auto& gcTree = counterpart.trees[*gcTreeIndex];
        REQUIRE(gcTree.sequences.size() == 1);
        CHECK(gcTree.sequences.front().frameCount == (kind == 42 ? 17 : 18));
        usize gcTextureRuns = 0;
        for (const auto& texture : counterpart.textureAnimations) {
            if (texture.name.find("ROAR") == std::string::npos)
                continue;
            CHECK(texture.offset == 0);
            CHECK(texture.frames == 18);
            ++gcTextureRuns;
        }
        CHECK(gcTextureRuns > 0);
        const auto gcMoves =
            formats::parseCritterWad(readFile(gc / "CRITTER" / (std::string(gcName) + ".WAD")));
        usize gcRoars = 0;
        for (const auto& move : gcMoves.moves) {
            if (move.name != "ROAR")
                continue;
            CHECK(move.sfxFrame == 20);
            ++gcRoars;
        }
        CHECK(gcRoars > 0);

        std::vector<std::string> warnings;
        auto original = loadLegendsN64Archive(root / nativeName, warnings, program, {}, true);
        auto adapted = loadLegendsBossAssets(device, root, gc, kind, program, {}, warnings);
        REQUIRE(original);
        REQUIRE(adapted);
        const auto originalIndex = original->trees.find("BOSSROARFX");
        const auto adaptedIndex = adapted->archive.trees.find("BOSSROARFX");
        REQUIRE(originalIndex);
        REQUIRE(adaptedIndex);
        const auto& raw = original->trees.tree(*originalIndex);
        const auto& translated = adapted->archive.trees.tree(*adaptedIndex);
        REQUIRE(raw.sequences.size() == 1);
        REQUIRE(translated.sequences.size() == 1);
        CHECK(raw.sequences.front().frames == 38); // General native loader is untouched.
        CHECK(translated.sequences.front().frames == 18);
        CHECK(raw.sequences.front().frameRate == translated.sequences.front().frameRate);
        REQUIRE(raw.nodes.size() == translated.nodes.size());
        usize shiftedRuns = 0;
        for (usize n = 0; n < raw.nodes.size(); ++n) {
            const auto& before = raw.nodes[n];
            const auto& after = translated.nodes[n];
            CHECK(before.position == after.position);
            CHECK(before.objectFlags == after.objectFlags);
            REQUIRE(before.objectFrames.size() == after.objectFrames.size());
            for (usize run = 0; run < before.objectFrames.size(); ++run) {
                CHECK(before.objectFrames[run].object == after.objectFrames[run].object);
                CHECK(before.objectFrames[run].frames == after.objectFrames[run].frames);
                if (before.objectFrames[run].frames > 1) {
                    CHECK(before.objectFrames[run].start == 0);
                    CHECK(after.objectFrames[run].start == -20);
                    ++shiftedRuns;
                }
            }
        }
        CHECK(shiftedRuns == (kind == 42 ? 5 : 8));
        // Independent transform oracle: new frame0/17 is the same native
        // geometry placement as raw frame20/37, not a stretched or moved effect.
        for (const s32 frame : {0, 17}) {
            TreePose before;
            TreePose after;
            before.evaluate(raw, 0, static_cast<f32>(frame + 20));
            after.evaluate(translated, 0, static_cast<f32>(frame));
            REQUIRE(before.matrices().size() == after.matrices().size());
            for (usize n = 0; n < before.matrices().size(); ++n)
                CHECK(before.matrices()[n] == after.matrices()[n]);
        }
    }
}

TEST_CASE("Native boss cues cannot borrow missing art while GDL shared effects remain available",
          "[legends][native-boss-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("N64 native banks not installed");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    ItemArchive weapons;
    REQUIRE(weapons.load(gc / "WEAPONS"));
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, gc, 34, readFile(root / "_program/game.bin"),
                                       {}, warnings);
    REQUIRE(stock);
    CombatCue cue;
    for (u32 i = 0; i < weapons.trees.size(); ++i) {
        const auto& tree = weapons.trees.tree(i);
        if (!stock->archive.trees.find(tree.name)) {
            cue.tree = tree.name;
            break;
        }
    }
    REQUIRE_FALSE(cue.tree.empty());
    CAPTURE(cue.tree);
    CHECK(CritterCueEmission::resolve(cue, &stock->archive, weapons).archive == &weapons);
    CHECK(CritterCueEmission::resolve(cue, &stock->archive, stock->archive).archive == nullptr);
    cue.tree = "FBALL_EXP";
    CHECK(CritterCueEmission::resolve(cue, &stock->archive, stock->archive).archive ==
          &stock->archive);
}

TEST_CASE("Native boss fire explosions retain their scoped weapons geometry and pixels",
          "[legends][native-boss-shared-effects][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "weapons/seq.wad"))
        SKIP("N64 native weapon bank not installed");
    const auto program = readFile(root / "_program/game.bin");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    std::vector<std::string> warnings;
    auto weapons = loadLegendsN64Archive(root / "weapons", warnings, program);
    REQUIRE(weapons);
    test::FakeRenderDevice device;
    for (const s32 kind : {34, 35, 43}) {
        auto stock = loadLegendsBossAssets(device, root, gc, kind, program, {}, warnings);
        REQUIRE(stock);
        CAPTURE(kind, stock->archive.models.size(), stock->archive.textures.size(),
                weapons->models.size(), weapons->textures.size());
        CHECK(stock->archive.models.size() > weapons->models.size());
        CHECK(stock->archive.textures.size() > weapons->textures.size());
        for (const std::string name : {"FBALL_EXP", "FBALL_EXP2"}) {
            CAPTURE(name);
            const auto sourceIndex = weapons->trees.find(name + ':');
            const auto actualIndex = stock->archive.trees.find(name);
            REQUIRE(sourceIndex);
            REQUIRE(actualIndex);
            const auto& source = weapons->trees.tree(*sourceIndex);
            const auto& actual = stock->archive.trees.tree(*actualIndex);
            REQUIRE(source.sequences.size() == 1);
            REQUIRE(actual.sequences.size() == 1);
            CHECK(actual.sequences.front().name.empty());
            CHECK(actual.sequences.front().frames == source.sequences.front().frames);
            CHECK(actual.sequences.front().frameRate == source.sequences.front().frameRate);
            REQUIRE(actual.nodes.size() == source.nodes.size());
            usize compared = 0;
            for (usize node = 0; node < source.nodes.size(); ++node) {
                const auto& expected = source.nodes[node];
                const auto& bound = actual.nodes[node];
                REQUIRE(bound.objectFrames.size() == expected.objectFrames.size());
                const auto compareRun = [&](const TreeNodeInfo::ObjectFrames& left,
                                            const TreeNodeInfo::ObjectFrames& right) {
                    REQUIRE(right.frames == left.frames);
                    REQUIRE(right.start == left.start);
                    const auto first = weapons->models.find(left.object);
                    const auto composed = stock->archive.models.find(right.object);
                    REQUIRE(first);
                    REQUIRE(composed);
                    for (s32 frame = 0; frame < left.frames; ++frame) {
                        const auto& a = weapons->models.mesh(*first + left.start + frame);
                        const auto& b = stock->archive.models.mesh(*composed + right.start + frame);
                        REQUIRE(b.vertices.size() == a.vertices.size());
                        CHECK(bool(b.vertices == a.vertices));
                        REQUIRE(b.parts.size() == a.parts.size());
                        for (usize part = 0; part < a.parts.size(); ++part) {
                            CHECK(bool(b.parts[part].indices == a.parts[part].indices));
                            const auto& sourceImage =
                                weapons->textures.image(a.parts[part].texture);
                            const auto& actualImage =
                                stock->archive.textures.image(b.parts[part].texture);
                            CHECK(actualImage.width == sourceImage.width);
                            CHECK(actualImage.height == sourceImage.height);
                            CHECK(bool(actualImage.pixels == sourceImage.pixels));
                            ++compared;
                        }
                    }
                };
                if (expected.objectFrames.empty()) {
                    compareRun({expected.object, 0, 1}, {bound.object, 0, 1});
                } else {
                    for (usize run = 0; run < expected.objectFrames.size(); ++run)
                        compareRun(expected.objectFrames[run], bound.objectFrames[run]);
                }
                REQUIRE(expected.textureAnimation >= 0);
                REQUIRE(bound.textureAnimation >= 0);
                const auto& sourceCycle =
                    weapons->trees.textureAnimations().at(expected.textureAnimation);
                const auto& boundCycle =
                    stock->archive.trees.textureAnimations().at(bound.textureAnimation);
                REQUIRE(boundCycle.frames == sourceCycle.frames);
                CHECK(boundCycle.rate == sourceCycle.rate);
                CHECK(boundCycle.start == sourceCycle.start);
                CHECK(boundCycle.offset == sourceCycle.offset);
                for (s32 frame = 0; frame < sourceCycle.frames; ++frame) {
                    const auto& sourceImage = weapons->textures.image(sourceCycle.source + frame);
                    const auto& actualImage =
                        stock->archive.textures.image(boundCycle.source + frame);
                    CHECK(actualImage.width == sourceImage.width);
                    CHECK(actualImage.height == sourceImage.height);
                    CHECK(bool(actualImage.pixels == sourceImage.pixels));
                }
            }
            CHECK(compared > 0);
        }
    }
}

TEST_CASE("Native shared effect composition preserves primary groups and named weapon spells",
          "[legends][native-boss-shared-effects][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "bosseffect/fire/seq.wad"))
        SKIP("N64 native fire bank not installed");
    const auto program = readFile(root / "_program/game.bin");
    std::vector<std::string> warnings;
    const std::array lenders{root / "bosseffect/fire", root / "weapons"};
    auto archive =
        loadLegendsN64Archive(root / "bosseffect/fire", warnings, program, {}, true, lenders);
    REQUIRE(archive);
    const auto loop = archive->trees.find("FBALL_LOOP");
    REQUIRE(loop);
    REQUIRE_FALSE(archive->trees.tree(*loop).nodes.empty());
    CHECK_FALSE(archive->trees.tree(*loop).nodes.front().object.starts_with("@N64SHARED/"));
    usize loopCount = 0;
    for (u32 i = 0; i < archive->trees.size(); ++i)
        loopCount += archive->trees.tree(i).name == "FBALL_LOOP";
    CHECK(loopCount == 1);
    // MAGIC's unrelated spell BODYs must not be merged into a pretend common
    // hierarchy just to expose the two zero-FOURCC explosion groups.
    CHECK(archive->trees.find("MAGIC:FIRE"));
    CHECK(archive->trees.find("MAGIC:ACID"));
    CHECK_FALSE(archive->trees.find("MAGIC"));
    CHECK(archive->trees.find("FBALL_EXP"));
    CHECK(archive->trees.find("FBALL_EXP2"));
}
TEST_CASE("Native boss effect submissions retain texture color, animated pixels and authored depth",
          "[legends][native-effect-material][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "bosseffect/acid/objects.wad"))
        SKIP("N64 native effect banks not installed");
    const auto program = readFile(root / "_program/game.bin");
    struct Sample {
        const char* bank;
        const char* tree;
        s32 frames;
        bool depth;
    };
    const std::array samples{
        Sample{"acid", "ABALL_LOOP", 15, false}, Sample{"acid", "ABALL_EXP", 15, false},
        Sample{"elec", "EBALL_LOOP", 17, false}, Sample{"elec", "EBALL_EXP", 15, false},
        Sample{"fire", "FBALL_LOOP", 15, true}};
    for (const auto& sample : samples) {
        DYNAMIC_SECTION(sample.tree) {
            test::FakeRenderDevice device;
            std::vector<std::string> warnings;
            auto archive = loadLegendsN64Archive(root / "bosseffect" / sample.bank, warnings,
                                                 program, {}, true);
            REQUIRE(archive);
            const auto tree = archive->trees.find(sample.tree);
            REQUIRE(tree);
            const auto& sequence = archive->trees.tree(*tree).sequences.front();
            CHECK(sequence.frames == sample.frames);
            CHECK(sequence.frameRate == 60); // rate32 -> 15fps, not 60fps.
            EffectTrees effects;
            EffectTrees::Setting setting;
            const bool loop = std::string_view(sample.tree).ends_with("LOOP");
            setting.seconds = loop ? 2.0f : 0.0f;
            REQUIRE(effects.startSet(device, *archive, sample.tree, Vec3{0, 0, 20}, setting));
            std::vector<u8> firstPixels;
            const WorldCamera camera;
            const auto clip = WorldCamera::projection(1.0f, 1.0f) * camera.view();
            for (const int ticks : {0, 12, 24}) {
                CAPTURE(ticks);
                if (ticks != 0)
                    for (int i = 0; i < 12; ++i)
                        effects.update(1.0f / 60);
                device.draws.clear();
                effects.draw(device, clip, WorldLighting{});
                REQUIRE_FALSE(device.draws.empty());
                for (const auto& draw : device.draws) {
                    REQUIRE_FALSE(draw.vertices.empty());
                    CHECK(draw.state.depthTest == sample.depth);
                    CHECK(draw.state.depthWrite == sample.depth);
                    for (const auto& vertex : draw.vertices)
                        CHECK(vertex.color == Color::white());
                    const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
                    REQUIRE(texture);
                    REQUIRE_FALSE(texture->pixels.empty());
                    if (ticks == 0)
                        firstPixels = texture->pixels;
                    else
                        CHECK(texture->pixels != firstPixels);
                    // Reversed Z: authored compare keeps a nearer opaque body in
                    // front of fire, without changing acid/electric's native policy.
                    const auto depthAt = [&](f32 z) {
                        const auto point = draw.transform * Vec4{0, 0, z, 1};
                        return point.z / point.w;
                    };
                    CHECK(depthAt(10) > depthAt(20));
                    CHECK(depthAt(20) > depthAt(30));
                    CHECK((!draw.state.depthTest || depthAt(20) >= depthAt(10)) == !sample.depth);
                    CHECK((!draw.state.depthTest || depthAt(20) >= depthAt(30)));
                }
            }
            if (!loop) {
                int elapsedTicks = 24;
                while (effects.count() != 0 && elapsedTicks < 120) {
                    effects.update(1.0f / 60);
                    ++elapsedTicks;
                }
                CHECK(effects.count() == 0);
                CHECK(elapsedTicks / 60.0f >= (sample.frames - 1) / 15.0f);
                CHECK(elapsedTicks / 60.0f <= sample.frames / 15.0f + 0.02f);
            }
        }
    }
}

TEST_CASE("Skorne shared projectile materials match their native banks after composition",
          "[legends][native-effect-material][native-skorn-render][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "bosseffect/acid/seq.wad"))
        SKIP("N64 native boss banks not installed");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(root / "_program/game.bin");
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, gc, 42, program, {}, warnings);
    REQUIRE(stock);
    for (const auto& [bank, name] :
         {std::pair{"acid", "ABALL_LOOP"}, std::pair{"elec", "EBALL_LOOP"}}) {
        CAPTURE(bank, name);
        // Independent native-byte oracle, not a second invocation of the model
        // or texture decoder. Both first frames are authored 8x8 RGBA32 pixels.
        const auto bytes = readFile(root / "bosseffect" / bank / "objects.wad");
        const auto be32 = [&](usize at) {
            REQUIRE(at + 4 <= bytes.size());
            return (u32{bytes[at]} << 24U) | (u32{bytes[at + 1]} << 16U) |
                   (u32{bytes[at + 2]} << 8U) | bytes[at + 3];
        };
        const auto section = [&](std::string_view tag) {
            const u32 directory = be32(0);
            const u32 count = be32(4);
            for (u32 i = 0; i < count; ++i) {
                const usize at = usize{directory} + usize{i} * 12;
                REQUIRE(at + 12 <= bytes.size());
                if (std::equal(tag.begin(), tag.end(), bytes.begin() + at))
                    return be32(at + 4);
            }
            FAIL("Native effect oracle section absent: " << tag);
            return u32{0};
        };
        const usize firstTexture = usize{section("TXHD")} + 15 * 24;
        REQUIRE(be32(firstTexture + 12) == 0x00080008U);
        REQUIRE(bytes[firstTexture + 18] == 0); // RDP RGBA
        REQUIRE(bytes[firstTexture + 19] == 3); // 32-bit pixels
        const usize firstPixels = usize{section("TEXR")} + be32(firstTexture);
        REQUIRE(firstPixels + 8 * 8 * 4 <= bytes.size());
        const std::vector<u8> nativePixels(bytes.begin() + firstPixels,
                                           bytes.begin() + firstPixels + 8 * 8 * 4);
        // The shipping boss composes independently keyed shared effects, unlike
        // the standalone combined-group material test above. Check both paths.
        auto source = loadLegendsN64Archive(root / "bosseffect" / bank, warnings, program);
        REQUIRE(source);
        EffectTrees nativeEffects;
        EffectTrees composedEffects;
        EffectTrees::Setting setting;
        setting.loop = true;
        setting.seconds = 3;
        const Vec3 position{5, 7, 20};
        REQUIRE(
            nativeEffects.startSet(device, *source, std::string(name) + ':', position, setting));
        REQUIRE(composedEffects.startSet(device, stock->archive, name, position, setting));
        const WorldCamera camera{.position = {15, 20, -30}, .pitch = .4f, .yaw = -.2f};
        const CameraFrame frame = CameraFrame::of(camera);
        const auto clip = WorldCamera::projection(1.0f, 1.5f) * camera.view();
        std::vector<u8> birthPixels;
        for (int tick = 0; tick <= 96; ++tick) {
            if (tick != 0) {
                nativeEffects.update(1.0f / 60);
                composedEffects.update(1.0f / 60);
            }
            if (tick != 1 && tick % 12 != 0)
                continue;
            CAPTURE(tick);
            device.draws.clear();
            nativeEffects.draw(device, clip, {}, &frame, 1);
            const auto expected = device.draws;
            REQUIRE_FALSE(expected.empty());
            device.draws.clear();
            composedEffects.draw(device, clip, {}, &frame, 1);
            REQUIRE(device.draws.size() == expected.size());
            for (usize i = 0; i < expected.size(); ++i) {
                const auto& actual = device.draws[i];
                CHECK(bool(actual.vertices == expected[i].vertices));
                CHECK(actual.transform == expected[i].transform);
                CHECK(actual.state.blend == expected[i].state.blend);
                CHECK(actual.state.depthTest == expected[i].state.depthTest);
                CHECK(actual.state.depthWrite == expected[i].state.depthWrite);
                CHECK_FALSE(actual.state.depthTest);
                CHECK_FALSE(actual.state.depthWrite);
                CHECK(actual.state.blend == BlendMode::Alpha);
                const auto* actualTexture = dynamic_cast<const test::FakeTexture*>(actual.texture);
                const auto* expectedTexture =
                    dynamic_cast<const test::FakeTexture*>(expected[i].texture);
                REQUIRE(actualTexture);
                REQUIRE(expectedTexture);
                CHECK(actualTexture->width() == expectedTexture->width());
                CHECK(actualTexture->height() == expectedTexture->height());
                CHECK(bool(actualTexture->pixels == expectedTexture->pixels));
                REQUIRE_FALSE(actualTexture->pixels.empty());
                if (tick == 0) {
                    birthPixels = actualTexture->pixels;
                    CHECK(bool(actualTexture->pixels == nativePixels));
                }
                if (tick == 12)
                    CHECK(bool(actualTexture->pixels != birthPixels));
                if (tick == 1)
                    INFO("Native " << name
                                   << " first-tick presentation blend=" << actual.state.textureBlend
                                   << ", next texture=" << (actual.state.nextTexture != nullptr));
                for (const auto& vertex : actual.vertices) {
                    // FCFFFFFF/FFFCF238 selects TEXEL0, not the authored black
                    // Vtx RGB. The full-camera mode must not multiply it back in.
                    CHECK(vertex.color == Color::white());
                    if (std::string_view(bank) == "acid") {
                        // Raw Vtx coordinates are +/-144/64 on X/Y, zero Z.
                        // e002754c mode3 uses camera * diag(-1,+1,-1), preserving
                        // their 4.5-world-unit size rather than screen-size scaling.
                        const Vec3 away = vertex.position - position;
                        CHECK(std::abs(glm::dot(away, frame.right)) ==
                              Catch::Approx(2.25f).margin(.0001f));
                        CHECK(std::abs(glm::dot(away, frame.up)) ==
                              Catch::Approx(2.25f).margin(.0001f));
                        CHECK(glm::dot(away, frame.forward) == Catch::Approx(0).margin(.0001f));
                    }
                }
            }
        }
    }
}

TEST_CASE("Native Skorne keeps its four-second wake and complete authored entrance clock",
          "[legends][native-boss-intro][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "boss1/seq.wad"))
        SKIP("N64 native boss banks not installed");
    const auto program = readFile(root / "_program/game.bin");
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    for (const s32 kind : {42, 43}) {
        DYNAMIC_SECTION(kind) {
            test::FakeRenderDevice device;
            std::vector<std::string> warnings;
            auto stock = loadLegendsBossAssets(device, root, gc, kind, program, {}, warnings);
            REQUIRE(stock);
            const auto start = stock->tree->findSequence("STRT");
            REQUIRE(start);
            const s32 frames = kind == 42 ? 51 : 120;
            CHECK(stock->tree->sequences[*start].frames == frames);
            CHECK(stock->tree->sequences[*start].frameRate == 30);
            const auto& entrance =
                stock->data.moves()[*stock->data.moveOfType(MoveDefinition::kStart)];
            if (kind == 42) {
                const auto continuation = stock->data.moveNamed("START2");
                REQUIRE(continuation);
                CHECK(entrance.link == *continuation);
                const auto& second = stock->data.moves()[*continuation];
                CHECK(second.type == MoveDefinition::kStart);
                CHECK(second.priority == 0xfff);
                CHECK(second.interrupt == 0);
                CHECK(second.link == -1);
                CHECK(second.sound == entrance.sound2);
                CHECK(second.soundFrame == 26);
                const auto nextSequence = stock->tree->findSequence(second.anim);
                REQUIRE(nextSequence);
                CHECK(stock->tree->sequences[*nextSequence].frames == 83);
                CHECK(stock->tree->sequences[*nextSequence].frameRate == 30);
            } else {
                CHECK(entrance.link == -1);
            }
            Combatant actor;
            REQUIRE(actor.spawn(*stock, 0, Vec3{0}, 0, nullptr, {}, 'F'));
            actor.draw(device, Mat4{1}, WorldLighting{});
            std::vector<Vec3> initialGeometry;
            for (const auto& draw : device.draws)
                for (const auto& vertex : draw.vertices)
                    initialGeometry.push_back(vertex.position);
            REQUIRE_FALSE(initialGeometry.empty());
            bool changedGeometry = false;
            int ticks = 0;
            while (!actor.moveDone() && ticks < 300) {
                actor.update(1, 1.0f / 60, {});
                ++ticks;
                // Repeated presentation samples never advance the native clock.
                for (const f32 alpha : {0.0f, 0.3f, 0.7f, 1.0f}) {
                    device.draws.clear();
                    actor.draw(device, Mat4{1}, WorldLighting{}, nullptr, nullptr, nullptr, alpha);
                }
                std::vector<Vec3> shown;
                for (const auto& draw : device.draws)
                    for (const auto& vertex : draw.vertices)
                        shown.push_back(vertex.position);
                changedGeometry |= shown != initialGeometry;
                CHECK(actor.moveType() == MoveDefinition::kStart);
            }
            CHECK(changedGeometry);
            CHECK(ticks / 60.0f >= (frames - 1) / 30.0f);
            CHECK(ticks / 60.0f <= frames / 30.0f + 0.02f);
            actor.clear();
            Bosses bosses;
            bosses.open(device, gc, nullptr, {}, 'F');
            REQUIRE(bosses.preload(kind, std::move(stock)));
            REQUIRE(bosses.spawn(kind, Vec3{0}, 0, 100));
            const std::array players{EnemyView{.player = 0, .position = Vec3{0}}};
            for (int tick = 0; tick < 210; ++tick)
                bosses.update(1, 1.0f / 60, players);
            CHECK_FALSE(bosses.view().awake);
            CHECK(bosses.moveName() == "INIT");
            CHECK_FALSE(bosses.fighter().moveDone());
            for (int tick = 0; tick < 29; ++tick)
                bosses.update(1, 1.0f / 60, players);
            CHECK_FALSE(bosses.view().awake); // 239/60 seconds: still before the native deadline.
            CHECK_FALSE(bosses.fighter().moveDone());
            for (int tick = 0; tick < 3; ++tick)
                bosses.update(1, 1.0f / 60, players);
            CHECK(bosses.view().awake);
            CHECK(bosses.moveName() == "START");
            CHECK_FALSE(bosses.fighter().moveDone());
            bosses.close();
        }
    }
    CombatantAssets defaults;
    CHECK(defaults.wakeDelay == 2);
    CHECK_FALSE(defaults.initialMoveBeforeWake);
    defaults.wakeDelay = 4;
    defaults.initialMoveBeforeWake = true;
    defaults.clear();
    CHECK(defaults.wakeDelay == 2);
    CHECK_FALSE(defaults.initialMoveBeforeWake);
}
} // namespace
