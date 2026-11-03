#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/audio/LegendsSoundTable.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/critters/CombatantCheckpoint.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct SkorneScene {
    std::filesystem::path root = test::assetOrSkip("PLAYERS").parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;
    std::shared_ptr<LegendsLevel> imported;
    std::vector<usize> capObjects;
    std::vector<Vec3> capVertices;

    explicit SkorneScene(s32 level = 1, bool settleEntrance = true,
                         std::string_view mapName = "boss2") {
        const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
        const auto directory = native / "levels" / mapName;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
            SKIP("Native N64 Skorne assets are not installed");
        const auto objects = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects,
                                                 readFile(directory / "ipr.rom"));
        std::set<std::array<f32, 3>> permanentVertices;
        for (const auto& object : map.objects) {
            if ((object.flags & 0x200U) != 0 && object.name.starts_with("ROCKFLY"))
                continue;
            for (const auto& vertex : object.mesh.vertices) {
                const Vec3 point{object.transform * Vec4{vertex.position, 1}};
                permanentVertices.insert({point.x, point.y, point.z});
            }
        }
        for (usize index = 0; index < map.objects.size(); ++index) {
            const auto& object = map.objects[index];
            if ((object.flags & 0x200U) != 0 && object.name.starts_with("ROCKFLY")) {
                capObjects.push_back(index);
                // Seams can share vertices with the permanent floor. Observe
                // each piece through a point exclusive to the cap geometry.
                std::optional<Vec3> marker;
                for (const auto& vertex : object.mesh.vertices) {
                    const Vec3 point{object.transform * Vec4{vertex.position, 1}};
                    if (!permanentVertices.contains({point.x, point.y, point.z})) {
                        marker = point;
                        break;
                    }
                }
                REQUIRE(marker);
                capVertices.push_back(*marker);
            }
        }
        for (auto& texture : formats::decodeLegendsN64Textures(objects))
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency});
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        REQUIRE(adapter);
        PartyMember member;
        member.player = 0;
        member.save.name = "TEST";
        member.save.progress().health = 2000;
        member.save.progress().experience = levelExperience(level);
        const std::array party{member};
        LegendsTraversal traversal;
        traversal.open(map, 0, member.save);
        imported = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
        imported->initializeParty(party);
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = traversal.start()}}, map.minBounds,
            map.maxBounds);
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = mapName;
        REQUIRE(world.loadImported(device, root, ref, std::move(layout), traversal.collision(),
                                   imported));
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        config.camera.compass = false;
        config.combat.autoMelee = false;
        context.config = &config;
        context.strings = &strings;
        context.unpackedRoot = root;
        PlayOptions options;
        options.position = traversal.start();
        options.yaw = traversal.actor().yaw();
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
        if (!settleEntrance)
            return;
        scene.setCheats(0, {.god = true});
        for (int tick = 0; tick < 1200; ++tick) {
            step();
            if (!scene.spawning() && imported->opponentsReady() && !scene.animator(0)->entering())
                break;
        }
        REQUIRE_FALSE(scene.spawning());
        REQUIRE(imported->opponentsReady());
        REQUIRE_FALSE(scene.animator(0)->entering());
        REQUIRE(scene.bosses().present());
        scene.bosses().wake();
        scene.setCheats(0, {});
    }

    void step() { REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running); }

    Vec3 floor(f32 x, f32 z) const {
        // The authored boss root is below the deck. Query the actual arena rather
        // than positioning a victim at an attack's already-sampled flight point.
        Vec3 result{x, 100, z};
        const auto contact = world.collision().floorAt(result, 0, 200);
        REQUIRE(contact);
        result.y = contact->y;
        return result;
    }

    std::optional<Vec3> groundedNear(const Vec3& centre, f32 reach, f32 halfHeight,
                                     const std::function<bool(const Vec3&)>& admit = {}) const {
        const auto& collision = world.collision();
        const auto& actor = *scene.actor(0);
        std::optional<Vec3> result;
        f32 nearest = std::numeric_limits<f32>::max();
        const auto extent = static_cast<s32>(std::ceil(reach));
        for (s32 x = -extent; x <= extent; ++x) {
            for (s32 z = -extent; z <= extent; ++z) {
                const f32 horizontal = glm::length(Vec2{static_cast<f32>(x), static_cast<f32>(z)});
                // Stay inside the native reach, without relying on the player's
                // contact-radius expansion or an unsupported footprint at an edge.
                if (horizontal > reach - actor.radius())
                    continue;
                Vec3 feet{centre.x + x, 100, centre.z + z};
                const auto support = collision.floorAt(feet, 0, 200);
                if (!support)
                    continue;
                feet.y = support->y;
                if (admit && !admit(feet))
                    continue;
                const f32 vertical = std::abs(feet.y + actor.height() * .5f - centre.y);
                if (vertical > halfHeight + actor.height() * .5f - .5f)
                    continue;
                const Vec3 body = feet + actor.followPoint() - actor.position() +
                                  Vec3{0, PlayerActor::kWallSphereLift, 0};
                if (glm::distance(collision.resolveSphere(body, actor.reach()), body) > .01f)
                    continue;
                bool supported = true;
                for (const auto offset :
                     {Vec3{actor.radius(), 0, 0}, Vec3{-actor.radius(), 0, 0},
                      Vec3{0, 0, actor.radius()}, Vec3{0, 0, -actor.radius()}}) {
                    const auto edge = collision.floorAt(feet + offset, .1f, .1f);
                    supported &= edge.has_value();
                }
                const f32 distance = horizontal + vertical;
                if (supported && distance < nearest) {
                    result = feet;
                    nearest = distance;
                }
            }
        }
        return result;
    }

    const CombatantAssets& stock() {
        const auto resources = scene.bosses().resources();
        REQUIRE(resources.size() == 1);
        return *resources.front();
    }

    CombatantCheckpoint::Actor bossState() {
        auto& fighter = const_cast<Combatant&>(scene.bosses().fighter());
        std::vector<CombatantCheckpoint::Resource> resources;
        for (auto* resource : scene.bosses().resources())
            resources.push_back({static_cast<u32>(resources.size() + 1), resource});
        CombatantCheckpoint checkpoints(fighter, resources, {.collision = &world.collision()});
        const auto state = checkpoints.checkpoint();
        REQUIRE(state);
        REQUIRE_FALSE(state->parts.empty());
        return state->parts.front().actor;
    }

    std::vector<Vec3> bossGeometry(f32 alpha = -1) {
        device.draws.clear();
        scene.bosses().fighter().draw(device, Mat4{1}, {}, nullptr, nullptr, nullptr, alpha);
        std::vector<Vec3> vertices;
        for (const auto& draw : device.draws)
            for (const auto& vertex : draw.vertices)
                vertices.push_back(vertex.position);
        return vertices;
    }

    usize submittedCapObjects() {
        device.draws.clear();
        imported->draw(device, Mat4{1}, scene.viewCamera(), TreeModel::Pass::Opaque, -1);
        imported->draw(device, Mat4{1}, scene.viewCamera(), TreeModel::Pass::Blended, -1);
        usize count = 0;
        for (const auto point : capVertices) {
            count += std::ranges::any_of(device.draws, [&](const auto& draw) {
                return std::ranges::any_of(draw.vertices, [&](const auto& vertex) {
                    return glm::distance(vertex.position, point) < .00001f;
                });
            });
        }
        return count;
    }

    bool visiblySubmits(const EffectTrees::Effect& effect) {
        REQUIRE(effect.archive == &stock().archive);
        REQUIRE(effect.tree);
        REQUIRE(effect.model);
        device.draws.clear();
        scene.render(device, Mat4{1}, 640, 448);
        const auto submitted = device.draws;
        device.draws.clear();
        // The scene has applied this effect's live texture motion. Identify the
        // actual native image it submitted, not just an effect name in a queue.
        const auto camera = CameraFrame::of(scene.viewCamera());
        const auto matrices = effect.pose.drawMatrices(effect.transform(), camera);
        effect.model->draw(device, Mat4{1}, Mat4{1}, {}, matrices, nullptr, effect.opacity());
        for (const auto& expected : device.draws) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(expected.texture);
            REQUIRE(texture);
            bool visibleTexel = false;
            for (usize alpha = 3; alpha < texture->pixels.size(); alpha += 4)
                visibleTexel |= texture->pixels[alpha] > 2;
            if (!visibleTexel)
                continue;
            for (const auto& actual : submitted) {
                if (actual.texture != expected.texture || actual.vertices.empty())
                    continue;
                CHECK(actual.state.blend == expected.state.blend);
                CHECK(actual.state.depthTest == expected.state.depthTest);
                CHECK(actual.state.depthWrite == expected.state.depthWrite);
                bool visibleVertex = false;
                for (const auto& vertex : actual.vertices) {
                    CHECK(std::isfinite(vertex.position.x));
                    CHECK(std::isfinite(vertex.position.y));
                    CHECK(std::isfinite(vertex.position.z));
                    visibleVertex |=
                        vertex.color.a > 2 &&
                        (vertex.color.r > 0 || vertex.color.g > 0 || vertex.color.b > 0);
                }
                if (visibleVertex)
                    return true;
            }
        }
        return false;
    }

    void forceMove(std::string_view name, f32 frame = 0) {
        auto& fighter = const_cast<Combatant&>(scene.bosses().fighter());
        std::vector<CombatantCheckpoint::Resource> resources;
        for (auto* resource : scene.bosses().resources())
            resources.push_back({static_cast<u32>(resources.size() + 1), resource});
        CombatantCheckpoint checkpoints(fighter, resources, {.collision = &world.collision()});
        auto state = checkpoints.checkpoint();
        REQUIRE(state);
        const auto& assets = stock();
        const auto index = assets.data.moveNamed(name);
        REQUIRE(index);
        const auto& move = assets.data.moves()[*index];
        const auto sequence = assets.tree->findSequence(move.anim);
        REQUIRE(sequence);
        auto& actor = state->parts.front().actor;
        actor.move = static_cast<s32>(*index);
        actor.pattern = -1;
        actor.forcedPattern = true;
        actor.moveDone = false;
        actor.finishedSeconds = 0;
        actor.moveTarget = actor.target = 0;
        actor.shotFrame = static_cast<s32>(frame) - 1;
        actor.soundsGiven = 0;
        actor.smoothValid = false;
        actor.areas.clear();
        actor.struckThisMove.clear();
        AnimationPlayer animation;
        animation.start(assets.tree->sequences[*sequence], *sequence, 0, frame);
        actor.player = animation.checkpoint();
        TreePose pose;
        pose.evaluate(*assets.tree, *sequence, frame);
        actor.pose = pose.checkpoint();
        REQUIRE(actor.pose);
        REQUIRE(checkpoints.restore(*state));
    }
};

TEST_CASE("Normal Skorne scene entry holds its cap then plays both entrance parts before READY",
          "[legends][native-skorn-gameplay][native-skorn-entry][assets]") {
    // No test wake, forced move, pose restore or bypass of player/camera arrival.
    SkorneScene test(1, false);
    auto& bosses = test.scene.bosses();
    REQUIRE(bosses.present());
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(bosses.moveName() == "INIT");
    const auto& stock = test.stock();
    REQUIRE(stock.initialMoveBeforeWake);
    const auto initialSequence = stock.tree->findSequence("INIT");
    const auto startSequence = stock.tree->findSequence("STRT");
    const auto secondSequence = stock.tree->findSequence("STR2");
    const auto readySequence = stock.tree->findSequence("REDY");
    REQUIRE(initialSequence);
    REQUIRE(startSequence);
    REQUIRE(secondSequence);
    REQUIRE(readySequence);
    const auto initial = test.bossState();
    CHECK(initial.player.sequence == *initialSequence);
    REQUIRE(stock.tree->sequences[*initialSequence].frames == 1);
    CHECK(initial.player.frame == 0);
    const auto initialGeometry = test.bossGeometry();
    REQUIRE_FALSE(initialGeometry.empty());
    REQUIRE(test.capObjects.size() == 43);
    CHECK(test.submittedCapObjects() == test.capObjects.size());
    for (const usize object : test.capObjects) {
        CHECK(test.imported->adapter().worldObjectVisible(object));
        CHECK(test.imported->adapter().worldObjectTransform(object) == Mat4{1});
    }

    int tick = 0;
    int startTick = -1;
    int secondTick = -1;
    int readyTick = -1;
    int attackTick = -1;
    f32 lastStartFrame = -1;
    f32 lastSecondFrame = -1;
    f32 lastReadyFrame = -1;
    bool startSound = false;
    bool secondSound = false;
    usize secondSoundAdmissions = 0;
    bool changedGeometry = false;
    bool movedCap = false;
    for (; tick < 1800; ++tick) {
        test.step();
        const auto state = test.bossState();
        const auto type = bosses.fighter().moveType();
        if (!bosses.view().awake) {
            CHECK(type == MoveDefinition::kInit);
            CHECK(state.player.sequence == *initialSequence);
            CHECK(state.player.frame == 0);
            for (const usize object : test.capObjects) {
                CHECK(test.imported->adapter().worldObjectVisible(object));
                CHECK(test.imported->adapter().worldObjectTransform(object) == Mat4{1});
            }
            if (tick % 30 == 0)
                CHECK(test.bossGeometry() == initialGeometry);
            continue;
        }
        if (startTick < 0) {
            startTick = tick;
            CHECK(type == MoveDefinition::kStart);
            CHECK(state.player.frame <= 1);
            CHECK(tick / 60.0f >= 4.0f - 0.02f);
        }
        if (type == MoveDefinition::kStart && state.player.sequence == *startSequence) {
            CHECK(readyTick == -1);
            CHECK(secondTick == -1);
            CHECK(state.player.frame >= lastStartFrame);
            lastStartFrame = state.player.frame;
            startSound |= (state.soundsGiven & 1U) != 0;
            for (const usize object : test.capObjects) {
                CHECK(test.imported->adapter().worldObjectVisible(object));
                movedCap |= test.imported->adapter().worldObjectTransform(object) != Mat4{1};
            }
            // The source's second START cue at frame76 is beyond STRT's 51 frames.
            CHECK((state.soundsGiven & 2U) == 0);
            if (tick % 15 == 0) {
                changedGeometry |= test.bossGeometry() != initialGeometry;
                for (const f32 alpha : {0.0f, 0.25f, 0.75f, 1.0f})
                    test.bossGeometry(alpha);
                CHECK(test.bossState().player == state.player);
            }
        } else if (type == MoveDefinition::kStart && state.player.sequence == *secondSequence) {
            if (secondTick < 0)
                secondTick = tick;
            CHECK(readyTick == -1);
            CHECK(state.player.frame >= lastSecondFrame);
            lastSecondFrame = state.player.frame;
            const bool sounding = (state.soundsGiven & 1U) != 0;
            if (sounding && !secondSound) {
                ++secondSoundAdmissions;
                CHECK(state.player.frame >= 26);
                CHECK(state.player.frame <= 27);
            }
            secondSound = sounding;
            CHECK((state.soundsGiven & 2U) == 0);
        } else if (type == MoveDefinition::kReady) {
            if (readyTick < 0)
                readyTick = tick;
            CHECK(state.player.sequence == *readySequence);
            lastReadyFrame = state.player.frame;
        } else {
            CHECK(readyTick >= 0);
            CHECK(type >= MoveDefinition::kAttackFrom);
            attackTick = tick;
            break;
        }
    }
    CAPTURE(startTick, secondTick, readyTick, attackTick, lastStartFrame, lastSecondFrame,
            lastReadyFrame);
    REQUIRE(startTick >= 0);
    REQUIRE(secondTick > startTick);
    REQUIRE(readyTick > secondTick);
    REQUIRE(attackTick > readyTick);
    CHECK(lastStartFrame == 50);
    CHECK(startSound);
    CHECK(secondSoundAdmissions == 1);
    CHECK(lastSecondFrame == 82);
    CHECK(changedGeometry);
    CHECK(movedCap);
    CHECK((secondTick - startTick) / 60.0f == Approx(51.0f / 30).margin(0.04f));
    CHECK((readyTick - secondTick) / 60.0f == Approx(83.0f / 30).margin(0.04f));
    CHECK(lastReadyFrame == 59);
    CHECK((attackTick - readyTick) / 60.0f >= 59.0f / 30);
    for (const usize object : test.capObjects)
        CHECK_FALSE(test.imported->adapter().worldObjectVisible(object));
    CHECK(test.submittedCapObjects() == 0);
}

TEST_CASE("Skorne native projectiles hit a grounded player through the real scene",
          "[legends][native-skorn-gameplay][assets]") {
    const s32 level = GENERATE(1, 99);
    for (const char* move : {"FBALL_L", "FBALL_R"}) {
        DYNAMIC_SECTION(move) {
            SkorneScene test(level);
            const Vec3 feet = test.floor(0, 24);
            REQUIRE(test.scene.relocate(feet));
            test.forceMove(move);
            const auto& definition = test.stock().data.moves()[*test.stock().data.moveNamed(move)];
            const auto* damage = test.stock().data.damage(definition.damage0);
            REQUIRE(damage);
            REQUIRE(damage->type == AttackDefinition::kProjectile);
            const auto* flight = test.stock().data.sound(damage->sound);
            const auto* impact = test.stock().data.sound(damage->hitSound);
            REQUIRE(flight);
            REQUIRE(impact);
            REQUIRE((flight->tree == "ABALL_LOOP" || flight->tree == "EBALL_LOOP"));
            REQUIRE((impact->tree == "ABALL_EXP" || impact->tree == "EBALL_EXP"));
            const auto health = test.scene.liveSave(0)->progress().health;
            f32 minimumHealth = static_cast<f32>(health);
            u32 flightId = 0;
            u32 impactId = 0;
            bool flightVisibleBeforeHit = false;
            bool impactVisible = false;
            for (int tick = 0; tick < 180; ++tick) {
                test.step();
                minimumHealth = std::min(
                    minimumHealth, static_cast<f32>(test.scene.liveSave(0)->progress().health));
                for (usize i = 0; i < test.scene.effects().count(); ++i) {
                    const auto& effect = test.scene.effects().effect(i);
                    if (effect.name == flight->tree && !flightVisibleBeforeHit &&
                        minimumHealth > health - 20) {
                        flightId = effect.id;
                        flightVisibleBeforeHit = test.visiblySubmits(effect);
                    } else if (effect.name == impact->tree && !impactVisible) {
                        impactId = effect.id;
                        CHECK(effect.scale == Approx(.72f));
                        impactVisible = test.visiblySubmits(effect);
                    }
                }
            }
            CAPTURE(move, level, feet.x, feet.y, feet.z, health, minimumHealth, flightId, impactId);
            CHECK(minimumHealth <= health - 20); // ordinary timed health drain is not a hit
            CHECK(flightId != 0);
            CHECK(impactId != 0);
            CHECK(flightId != impactId);
            CHECK(flightVisibleBeforeHit);
            CHECK(impactVisible);
            const auto current = test.scene.actor(0)->position();
            CHECK(test.world.collision().floorAt(current, 1, 1).has_value());
        }
    }
}

TEST_CASE("Skorne ground punches knock down a real player standing on the arena",
          "[legends][native-skorn-gameplay][assets]") {
    const s32 level = GENERATE(1, 99);
    const std::string name = GENERATE("POUNDL", "POUNDR");
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    SkorneScene test(level);
    const auto& move = test.stock().data.moves()[*test.stock().data.moveNamed(name)];
    test.forceMove(name, static_cast<f32>(move.frameStart));
    const auto* damage = test.stock().data.damage(move.damage0);
    REQUIRE(damage);
    REQUIRE(damage->type == AttackDefinition::kRing);
    REQUIRE(damage->maxDistance == 15);
    const auto* cue = test.stock().data.sound(damage->sound);
    REQUIRE(cue);
    CHECK(cue->tree == (name == "POUNDL" ? "BOSSATK6FX" : "BOSSATK7FX"));
    CHECK(cue->soundFormat == (name == "POUNDL" ? "S_SKORN1ATTCK6" : "S_SKORN1ATTCK7"));
    CHECK((cue->flags & 2U) != 0);
    const auto hand = test.scene.bosses().nodeTransform(move.colnode);
    REQUIRE(hand);
    const Vec3 centre{(*hand)[3]};
    // The hand lands beside the walking deck; the authored expanding wave,
    // not the wrist's exact XZ projection, reaches nearby grounded players.
    const auto supported =
        test.groundedNear(centre, damage->maxDistance, test.stock().data.rules().areaHalfHeight);
    CAPTURE(centre.x, centre.y, centre.z);
    REQUIRE(supported);
    const Vec3 feet = *supported;
    REQUIRE(test.scene.relocate(feet));
    const auto health = test.scene.liveSave(0)->progress().health;
    if constexpr (GDL_LEGENDS_AUDIO) {
        // Drain arrival cues before observing the punch's own native sound.
        test.imported->playSounds(output, feet);
        test.imported->stopSounds();
        output.stopAll();
    }
    bool knocked = false;
    bool visible = false;
    bool shook = false;
    bool sounded = false;
    for (int tick = 0; tick < 120; ++tick) {
        test.step();
        const auto& player = test.scene.participants().front();
        knocked |= player.reaction == PlayerDeed::FallBack ||
                   player.reaction == PlayerDeed::FallForward || test.scene.animator(0)->floored();
        for (usize i = 0; i < test.scene.effects().count() && !visible; ++i) {
            const auto& effect = test.scene.effects().effect(i);
            if (effect.name == cue->tree)
                visible = test.visiblySubmits(effect);
        }
        const auto baseCamera = test.imported->camera();
        REQUIRE(baseCamera);
        const auto shown = test.scene.viewCamera();
        shook |= glm::distance(baseCamera->position, shown.position) > .01f ||
                 std::abs(baseCamera->yaw - shown.yaw) > .0001f ||
                 std::abs(baseCamera->pitch - shown.pitch) > .0001f;
        if constexpr (GDL_LEGENDS_AUDIO) {
            test.imported->playSounds(output, feet);
            sounded |= output.voiceCount() > 0;
            std::array<f32, 1600> samples{};
            mixer.mix(samples);
            output.update();
        }
    }
    CAPTURE(feet.x, feet.y, feet.z, health, test.scene.liveSave(0)->progress().health);
    CHECK(test.scene.liveSave(0)->progress().health <= health - 20);
    CHECK(knocked);
    CHECK(visible);
    CHECK(shook);
    if constexpr (GDL_LEGENDS_AUDIO)
        CHECK(sounded);
}

TEST_CASE("Skorne naturally selects a damaging ground punch after the complete entrance",
          "[legends][native-skorn-gameplay][native-skorn-entry][assets]") {
    const std::string name = "POUNDR";
    SkorneScene test(1, false);
    auto& bosses = test.scene.bosses();
    for (int tick = 0; tick < 1800 && bosses.fighter().moveType() != MoveDefinition::kReady; ++tick)
        test.step();
    REQUIRE(bosses.fighter().moveType() == MoveDefinition::kReady);
    REQUIRE(bosses.view().awake);
    const auto state = test.bossState();
    const auto& stock = test.stock();
    const Mat4 root = glm::scale(
        glm::rotate(glm::translate(Mat4{1}, state.position + Vec3{0, stock.data.floorOffset(), 0}),
                    state.yaw, Vec3{0, 1, 0}),
        Vec3{state.scale * state.shrink});
    const Vec3 origin{root * Vec4{stock.data.originOffset(), 1}};
    const auto& move = stock.data.moves()[*stock.data.moveNamed(name)];
    const auto* damage = stock.data.damage(move.damage0);
    REQUIRE(damage);
    const auto* cue = stock.data.sound(damage->sound);
    REQUIRE(cue);
    const auto sequence = stock.tree->findSequence(move.anim);
    const auto wrist = stock.tree->findNode(move.colnode, kCombatantNodeNameLength);
    REQUIRE(sequence);
    REQUIRE(wrist);
    // A read-only pose predicts the hand's contact footprint; it never
    // selects/restores a move or advances the real actor's animation.
    TreePose pose;
    pose.evaluate(*stock.tree, *sequence, static_cast<f32>(move.frameStart));
    const Vec3 centre{(root * pose.matrices()[*wrist])[3]};
    const auto feet = test.groundedNear(
        centre, damage->maxDistance, stock.data.rules().areaHalfHeight, [&](const Vec3& point) {
            const auto delta = point - origin;
            const f32 distance = glm::length(Vec2{delta.x, delta.z});
            // Beyond claws/grab, inside an authored punch cone. The production
            // AI remains free to choose either side as earlier attacks move
            // the victim; the test never requests a move or resets their feet.
            return distance > 26 && distance < 34 &&
                   move.target.allows(distance, std::atan2(delta.x, delta.z) - state.yaw, 0);
        });
    CAPTURE(name, centre.x, centre.y, centre.z);
    REQUIRE(feet);
    REQUIRE(test.scene.relocate(*feet));
    const auto health = test.scene.liveSave(0)->progress().health;
    bool selected = false;
    bool knocked = false;
    bool visible = false;
    bool waveBorn = false;
    bool damaged = false;
    auto punchHealth = health;
    const MoveDefinition* active = nullptr;
    const CombatEffectDefinition* activeCue = nullptr;
    std::string chosen;
    std::string previous;
    std::string history;
    for (int tick = 0; tick < 600; ++tick) {
        const auto beforeHealth = test.scene.liveSave(0)->progress().health;
        test.step();
        const bool entered = bosses.moveName() != previous;
        if (entered) {
            previous = bosses.moveName();
            if (!history.empty())
                history += ',';
            history += previous;
        }
        if ((bosses.moveName() == "POUNDL" || bosses.moveName() == "POUNDR") && entered) {
            selected = true;
            chosen = bosses.moveName();
            active = &stock.data.moves()[*stock.data.moveNamed(chosen)];
            const auto* harm = stock.data.damage(active->damage0);
            REQUIRE(harm);
            activeCue = stock.data.sound(harm->sound);
            REQUIRE(activeCue);
            punchHealth = test.scene.liveSave(0)->progress().health;
            waveBorn = visible = knocked = false;
        }
        for (usize i = 0; i < test.scene.effects().count() && !visible && activeCue != nullptr &&
                          bosses.moveName() == chosen;
             ++i) {
            const auto& effect = test.scene.effects().effect(i);
            if (effect.name == activeCue->tree) {
                if (!waveBorn) {
                    CHECK(bosses.moveName() == chosen);
                    const auto impact = test.bossState();
                    CHECK(impact.player.frame >= static_cast<f32>(active->frameStart));
                    CHECK(impact.player.frame < static_cast<f32>(active->frameStart + 1));
                    waveBorn = true;
                }
                visible = test.visiblySubmits(effect);
            }
        }
        if (waveBorn && bosses.moveName() == chosen &&
            test.scene.liveSave(0)->progress().health <= beforeHealth - 20) {
            // Prior attacks and passive health drain cannot satisfy the test:
            // observe a new substantial hit within this punch's own window.
            CHECK(test.scene.liveSave(0)->progress().health <= punchHealth - 20);
            const auto& player = test.scene.participants().front();
            knocked = player.reaction == PlayerDeed::FallBack ||
                      player.reaction == PlayerDeed::FallForward ||
                      test.scene.animator(0)->floored();
            damaged = true;
            break;
        }
    }
    CAPTURE(chosen, history, feet->x, feet->y, feet->z, health, punchHealth,
            test.scene.liveSave(0)->progress().health);
    CHECK(selected);
    CHECK((chosen == "POUNDL" || chosen == "POUNDR"));
    CHECK(waveBorn);
    CHECK(damaged);
    CHECK(knocked);
    CHECK(visible);
}

TEST_CASE("Skorne releases a captured player back onto the native arena",
          "[legends][native-skorn-gameplay][native-capture][assets]") {
    SkorneScene test;
    const auto& move = test.stock().data.moves()[*test.stock().data.moveNamed("GRAB")];
    const auto* damage = test.stock().data.damage(move.damage0);
    REQUIRE(damage);
    REQUIRE(damage->type == AttackDefinition::kGrab);
    REQUIRE(move.frameStart == 12);
    REQUIRE(move.frameEnd == 15);
    REQUIRE(damage->offset == Vec3{0, -3, 0});
    std::optional<Vec3> feet;
    Vec3 centre{0};
    for (s32 frame = move.frameStart; frame <= move.frameEnd; ++frame) {
        test.forceMove("GRAB", static_cast<f32>(frame));
        const auto hand = test.scene.bosses().nodeTransform(move.colnode);
        REQUIRE(hand);
        centre = Vec3{(*hand)[3]} + damage->offset * test.scene.bosses().fighter().scale();
        feet = test.groundedNear(centre, damage->maxDistance, damage->maxDistance);
        if (feet)
            break;
    }
    CAPTURE(centre.x, centre.y, centre.z);
    REQUIRE(feet);
    // Advance into the complete native contact window from its preceding pose.
    // AnimationPlayer advances before contacts: starting at12 would round the
    // first half-frame to13 and skip the wrist position used to find this floor.
    // Never place the victim in the air at the hand's sampled height.
    test.forceMove("GRAB", static_cast<f32>(move.frameStart - 1));
    REQUIRE(test.scene.relocate(*feet));
    bool held = false;
    bool released = false;
    for (int tick = 0; tick < 600; ++tick) {
        test.step();
        const auto& capture = test.scene.participants().front().capture;
        if (!held && capture.held()) {
            auto& fighter = const_cast<Combatant&>(test.scene.bosses().fighter());
            const std::array resources{
                CombatantCheckpoint::Resource{1, test.scene.bosses().resources().front()}};
            CombatantCheckpoint checkpoints(fighter, resources,
                                            {.collision = &test.world.collision()});
            const auto sampled = checkpoints.checkpoint();
            REQUIRE(sampled);
            const auto& saved = sampled->parts.front().actor;
            CHECK(saved.player.frame >= move.frameStart);
            CHECK(saved.player.frame <= move.frameEnd);
            CHECK(saved.grabbed == 0);
        }
        held |= capture.held();
        if (held && !capture.active()) {
            released = true;
            break;
        }
    }
    const auto position = test.scene.actor(0)->position();
    CAPTURE(feet->x, feet->y, feet->z, position.x, position.y, position.z);
    REQUIRE(held);
    REQUIRE(released);
    CHECK(test.world.collision().floorAt(position, 1, 1).has_value());
    CHECK(glm::length(Vec2{position.x - feet->x, position.z - feet->z}) < 3);
    // Landing clears capture during PartyMotion. Exercise the ordinary imported
    // movement path on following ticks too: it must not re-sweep from the wrist
    // or leave a merely-nearby floor behind and resume falling.
    for (int tick = 0; tick < 120; ++tick) {
        test.step();
        REQUIRE_FALSE(test.scene.participants().front().capture.active());
        const auto settled = test.scene.actor(0)->position();
        CAPTURE(tick, settled.x, settled.y, settled.z);
        REQUIRE(std::isfinite(settled.x));
        REQUIRE(std::isfinite(settled.y));
        REQUIRE(std::isfinite(settled.z));
        const auto support = test.world.collision().floorAt(settled, 1, 1);
        REQUIRE(support);
        CHECK(settled.y == Approx(support->y).margin(.001f));
        CHECK(glm::length(Vec2{settled.x - feet->x, settled.z - feet->z}) < 3);
    }
}
TEST_CASE("Skorne roar visuals accompany the stretch through the real scene",
          "[legends][native-skorn-render][native-skorn-gameplay][assets]") {
    SkorneScene test;
    const auto& stock = test.stock();
    const auto move = stock.data.moveNamed("ROAR");
    REQUIRE(move);
    const auto& definition = stock.data.moves()[*move];
    REQUIRE(definition.soundFrame == 20);
    const auto* cue = stock.data.sound(definition.sound);
    REQUIRE(cue);
    REQUIRE(cue->tree == "BOSSROARFX");
    const auto findRoar = [&]() -> const EffectTrees::Effect* {
        for (usize i = 0; i < test.scene.effects().count(); ++i) {
            const auto& effect = test.scene.effects().effect(i);
            if (effect.name == "BOSSROARFX")
                return &effect;
        }
        return nullptr;
    };
    REQUIRE(findRoar() == nullptr);
    test.forceMove("ROAR");
    s32 bornAt = -1;
    s32 visibleAt = -1;
    bool submittedVisible = false;
    u32 instance = 0;
    for (s32 tick = 1; tick <= 60; ++tick) {
        test.step();
        const auto* effect = findRoar();
        if (effect == nullptr)
            continue;
        if (bornAt < 0) {
            bornAt = tick;
            instance = effect->id;
            CHECK(effect->archive == &stock.archive);
            REQUIRE(effect->tree);
            CHECK(effect->tree->name == "BOSSROARFX");
            CHECK(effect->player.frame() <= 1);
        }
        CHECK(effect->id == instance);
        test.device.draws.clear();
        test.scene.render(test.device, Mat4{1}, 640, 448);
        const auto sceneDraws = test.device.draws;
        test.device.draws.clear();
        const auto camera = CameraFrame::at({0, 40, -40});
        const auto matrices = effect->pose.drawMatrices(effect->transform(), camera);
        effect->model->draw(test.device, Mat4{1}, Mat4{1}, {}, matrices, nullptr,
                            effect->opacity());
        for (const auto& draw : test.device.draws) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            REQUIRE(texture);
            bool opaqueTexel = false;
            for (usize alpha = 3; alpha < texture->pixels.size(); alpha += 4)
                opaqueTexel |= texture->pixels[alpha] > 2;
            if (!opaqueTexel || draw.vertices.empty())
                continue;
            if (visibleAt < 0)
                visibleAt = tick;
            submittedVisible |= std::ranges::any_of(sceneDraws, [&](const auto& actual) {
                return actual.texture == draw.texture && !actual.vertices.empty();
            });
        }
    }
    // The move is 30fps: dispatch at20 means about40 fixed60Hz updates,
    // not the later area-damage frame or the effect's own texture delay20.
    CAPTURE(bornAt, visibleAt);
    REQUIRE(bornAt >= 39);
    REQUIRE(bornAt <= 41);
    REQUIRE(findRoar());
    CHECK(findRoar()->player.frame() > 0);
    // The GDL-compatible imported effect removes only the native20-frame
    // pre-roll. Its first actual image is transparent, the next is visible:
    // require pixels during the stretch, not merely an effect handle at20.
    CHECK(visibleAt >= bornAt);
    CHECK(visibleAt <= bornAt + 4);
    CHECK(submittedVisible);
    // Rendering the actual scene must use that native effect, not a body-only
    // submission or a GameCube effect substitute.
    test.device.draws.clear();
    const auto& shown = *findRoar();
    const auto camera = CameraFrame::at({0, 40, -40});
    const auto matrices = shown.pose.drawMatrices(shown.transform(), camera);
    shown.model->draw(test.device, Mat4{1}, Mat4{1}, {}, matrices, nullptr, shown.opacity());
    REQUIRE_FALSE(test.device.draws.empty());
    std::set<const Texture*> nativeTextures;
    for (const auto& draw : test.device.draws)
        nativeTextures.insert(draw.texture);
    test.device.draws.clear();
    test.scene.render(test.device, Mat4{1}, 640, 448);
    CHECK(std::ranges::any_of(test.device.draws, [&](const auto& draw) {
        return nativeTextures.contains(draw.texture) && !draw.vertices.empty();
    }));
    for (s32 tick = 0; tick < 30; ++tick)
        test.step();
    CHECK(findRoar() == nullptr); // The single visible phase does not loop or restart.
}
TEST_CASE("Hell2 enters through its native initial pose and completes START before turning",
          "[legends][native-hell2][native-skorn-gameplay][assets]") {
    SkorneScene test(1, false, "hell2");
    test.scene.setCheats(0, {.god = true});
    auto& bosses = test.scene.bosses();
    REQUIRE(bosses.present());
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(bosses.moveName() == "INIT");
    const auto& stock = test.stock();
    const auto initial = stock.tree->findSequence("INIT");
    const auto start = stock.tree->findSequence("STRT");
    const auto ready = stock.tree->findSequence("REDY");
    REQUIRE(initial);
    REQUIRE(start);
    REQUIRE(ready);
    REQUIRE(stock.tree->sequences[*initial].frames == 1);
    const auto first = test.bossState();
    REQUIRE(first.player.sequence == *initial);
    REQUIRE_FALSE(test.bossGeometry().empty());
    s32 startTick = -1;
    s32 readyTick = -1;
    s32 dormantCameraChecks = 0;
    f32 startLastFrame = -1;
    bool moved = false;
    const auto initialGeometry = test.bossGeometry();
    for (s32 tick = 0; tick < 1200; ++tick) {
        test.step();
        const auto state = test.bossState();
        CAPTURE(tick, bosses.moveName(), state.player.frame, state.position.y, state.yaw);
        if (!bosses.view().awake) {
            CHECK(state.player.sequence == *initial);
            CHECK(state.player.frame == 0);
            CHECK(state.yaw == first.yaw);
            if (test.imported->opponentsReady() && !test.scene.animator(0)->entering()) {
                const auto attention = test.imported->cameraAttention();
                const auto camera = test.imported->camera();
                REQUIRE(attention);
                REQUIRE(camera);
                const Vec3 expected = test.scene.actor(0)->position() + camera->up() * 3.0f;
                CHECK(glm::distance(*attention, expected) < .05f);
                ++dormantCameraChecks;
            }
            continue;
        }
        if (startTick < 0) {
            startTick = tick;
            CHECK(bosses.moveName() == "START");
            CHECK(state.player.frame <= 1);
        }
        if (bosses.moveName() == "START") {
            CHECK(state.player.sequence == *start);
            CHECK(state.yaw == first.yaw); // Native START turn-rate is zero.
            CHECK(state.player.frame >= startLastFrame);
            startLastFrame = state.player.frame;
            if (tick % 15 == 0)
                moved |= test.bossGeometry() != initialGeometry;
        } else if (bosses.fighter().moveType() == MoveDefinition::kReady) {
            readyTick = tick;
            CHECK(state.player.sequence == *ready);
            break;
        } else {
            FAIL("Hell2 selected an attack before completing its entrance");
        }
    }
    CAPTURE(startTick, readyTick, startLastFrame, stock.tree->sequences[*start].frames);
    REQUIRE(startTick >= 0);
    REQUIRE(readyTick > startTick);
    CHECK(dormantCameraChecks > 0);
    CHECK(startLastFrame >= stock.tree->sequences[*start].frames - 2);
    CHECK(moved);
}

TEST_CASE("Hell2 ranged chains leave claw and grab to their close-range admission gates",
          "[legends][native-hell2][native-skorn-gameplay][assets]") {
    SkorneScene test(1, false, "hell2");
    test.scene.setCheats(0, {.god = true});
    auto& bosses = test.scene.bosses();
    for (s32 tick = 0; tick < 1200 && bosses.fighter().moveType() != MoveDefinition::kReady; ++tick)
        test.step();
    REQUIRE(bosses.fighter().moveType() == MoveDefinition::kReady);
    const auto& stock = test.stock();
    for (const auto name : {"CLAWL", "GRAB"}) {
        const auto index = stock.data.moveNamed(name);
        REQUIRE(index);
        const auto& move = stock.data.moves()[*index];
        CHECK(move.target.maxDistance == 25);
        CHECK(move.target.allows(20, move.target.yaw, 0));
        CHECK_FALSE(move.target.allows(40, move.target.yaw, 0));
        CHECK((move.flags & MoveDefinition::kLinkedOnly) == 0);
        for (const auto& pattern : stock.data.patterns())
            CHECK(std::ranges::find(pattern.moves, static_cast<s32>(*index)) ==
                  pattern.moves.end());
    }
    const auto state = test.bossState();
    const auto feet =
        test.groundedNear(state.position + Vec3{0, 35, 45}, 18, 100, [&](const Vec3& point) {
            const Vec3 delta = point - state.position;
            return glm::length(Vec2{delta.x, delta.z}) > 35 &&
                   std::abs(std::atan2(delta.x, delta.z) - state.yaw) < .2f;
        });
    REQUIRE(feet);
    REQUIRE(test.scene.relocate(*feet));
    std::set<s32> patterns;
    std::set<std::string> attacks;
    for (s32 tick = 0; tick < 1800; ++tick) {
        test.step();
        const auto current = test.bossState();
        const Vec3 delta = test.scene.actor(0)->position() - current.position;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        CAPTURE(tick, bosses.moveName(), current.pattern, distance);
        REQUIRE(distance > 25);
        REQUIRE(bosses.moveName() != "CLAWL");
        REQUIRE(bosses.moveName() != "GRAB");
        if (current.pattern >= 0)
            patterns.insert(current.pattern);
        if (bosses.fighter().moveType() >= MoveDefinition::kAttackFrom)
            attacks.emplace(bosses.moveName());
    }
    CHECK(patterns.contains(1));
    CHECK(patterns.contains(2));
    CHECK(attacks.contains("FBALL_L"));
    CHECK(attacks.contains("FBALL_R"));
}
TEST_CASE("Hell2 corrected native attack names produce audible scene cues at their authored frames",
          "[legends][native-hell2][native-skorn-gameplay][audio][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    const auto name = GENERATE("SPIKES", "WING_ATTACK", "ROAR");
    SkorneScene test(1, true, "hell2");
    test.scene.setCheats(0, {.god = true});
    const auto& stock = test.stock();
    const auto index = stock.data.moveNamed(name);
    REQUIRE(index);
    const auto& move = stock.data.moves()[*index];
    const auto* cue = stock.data.sound(move.sound);
    REQUIRE(cue);
    const std::string_view expected = name == std::string_view{"SPIKES"}        ? "S_SKORN9ATTCK9"
                                      : name == std::string_view{"WING_ATTACK"} ? "S_SKORN9ATTCK10"
                                                                                : "S_SKORN2PROAR";
    REQUIRE(cue->soundFormat == expected);
    const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto routing = decodeLegendsSoundTable(readFile(native / "_program/game.bin"));
    const u32 row = name == std::string_view{"SPIKES"}        ? 50U
                    : name == std::string_view{"WING_ATTACK"} ? 51U
                                                              : 55U;
    const u16 macro = name == std::string_view{"SPIKES"}        ? u16{134}
                      : name == std::string_view{"WING_ATTACK"} ? u16{125}
                                                                : u16{132};
    CHECK(legendsSoundToken(routing, expected) == (0x10000U | row));
    REQUIRE(row < routing.size());
    // Named-only rows have no numeric input token. Their replacement is the
    // native effect ID reached through the 0x10000 | row lookup above.
    CHECK(routing[row].token == 0xffff);
    CHECK(routing[row].replacement == macro);
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    test.imported->playSounds(output, Vec3{10000});
    test.imported->stopSounds();
    output.stopAll();
    std::array<f32, 1600> samples{};
    mixer.mix(samples);
    mixer.mix(samples);
    output.update();
    test.forceMove(name);
    s32 firstAudible = -1;
    f64 energy = 0;
    for (s32 tick = 0; tick < 80; ++tick) {
        test.step();
        // A camera outside positional range must not attenuate native boss cues.
        test.imported->playSounds(output, Vec3{10000});
        mixer.mix(samples);
        output.update();
        for (const f32 value : samples) {
            CHECK(std::isfinite(value));
            energy += static_cast<f64>(value) * value;
            if (std::abs(value) > .00001f && firstAudible < 0)
                firstAudible = tick;
        }
    }
    CAPTURE(name, move.soundFrame, firstAudible, energy);
    REQUIRE(firstAudible >= 0);
    CHECK(firstAudible >= move.soundFrame * 2 - 2);
    CHECK(firstAudible <= move.soundFrame * 2 + 12);
    CHECK(energy > .001);
}
} // namespace
