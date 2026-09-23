#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"
#include "formats/CritterWad.h"
#include "game/enemies/CritterData.h"
#include "game/enemies/Critters.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;
constexpr f32 kPi = std::numbers::pi_v<f32>;

std::filesystem::path unpackedRoot() {
    return test::unpackedOrSkip("critter/GOLEM.json").parent_path().parent_path();
}

EnemyView playerAt(const Vec3& position, s32 player = 0) {
    EnemyView view;
    view.player = player;
    view.position = position;
    view.radius = 1.0f;
    view.height = 6.0f;
    return view;
}

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    out.object = 0;
    return out;
}

/** A flat floor sixty across at y 0. */
std::vector<CollisionTriangle> floor() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    return {
        triangle({-60, 0, -60}, {60, 0, -60}, {60, 0, 60}, up),
        triangle({-60, 0, -60}, {60, 0, 60}, {-60, 0, 60}, up),
    };
}

TEST_CASE("a critter wad holds the creature's type, moves, damages, parts and sounds",
          "[formats][assets]") {
    const std::filesystem::path wad = test::assetOrSkip("CRITTER/GOLEM.WAD");
    const formats::CritterFile file = formats::parseCritterWad(readFile(wad));
    REQUIRE(file.descriptors.size() == 1);
    REQUIRE(file.descriptors[0].name == "golem");
    REQUIRE(file.descriptors[0].prefix == "GOLEM");
    REQUIRE(file.descriptors[0].type == 3);
    REQUIRE(file.types.size() == 1);
    const formats::CritterTypeRecord& type = file.types[0];
    REQUIRE(type.suffix == "1");
    REQUIRE(type.radius == 4.0f);
    REQUIRE(type.wallRadius == 2.5f);
    REQUIRE(type.maxHealth == 400.0f);
    REQUIRE(type.expValue == 250.0f);
    REQUIRE(type.armor == 3.0f);
    REQUIRE(type.moveCount == 17);
    REQUIRE(type.colCount == 6);
    REQUIRE(type.originOffset[1] == 4.0f);
    REQUIRE(file.moves.size() == 17);
    REQUIRE(file.moves[10].name == "WALK");
    REQUIRE(file.moves[10].speed == 5.0f);
    REQUIRE(file.moves[10].target.minDistance == 7.0f);
    REQUIRE(file.moves[12].name == "ATTACK1L");
    REQUIRE(file.moves[12].frameStart == 6);
    REQUIRE(file.moves[12].frameEnd == 7);
    REQUIRE(file.moves[12].damage0 == 1);
    REQUIRE(file.moves[12].link == 13);
    REQUIRE(file.moves[12].colnode == "BALL");
    REQUIRE(file.damages.size() == 4);
    REQUIRE(file.damages[1].damage == 10.0f);
    REQUIRE(file.damages[3].maxDistance == 10.0f);
    REQUIRE(file.nodes.size() == 6);
    REQUIRE(file.nodes[5].nodeName == "HEAD");
    REQUIRE(file.nodes[5].radius == 3.0f);
    REQUIRE(file.sounds.size() == 11);
    REQUIRE(file.sounds[0].name == "HITDIE");
}

TEST_CASE("critter data reads a creature's table, clearing the packing tool's leftovers",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    CritterData golem;
    REQUIRE(golem.load(root / "critter/GOLEM.json"));
    REQUIRE(golem.loaded());
    REQUIRE(golem.kind() == kGolemCritter);
    REQUIRE(golem.folder() == "golem");
    REQUIRE(golem.tree() == "GOLEM1");
    REQUIRE(golem.maxHealth() == 400.0f);
    REQUIRE(golem.experience() == 250.0f);
    REQUIRE(golem.armor() == 3.0f);
    REQUIRE(golem.radius() == 4.0f);
    REQUIRE(golem.moves().size() == 17);
    REQUIRE(golem.parts().size() == 6);
    const auto walk = golem.moveNamed("WALK");
    REQUIRE(walk.has_value());
    REQUIRE(golem.moves()[*walk].type == CritterMove::kWalk);
    REQUIRE(golem.moves()[*walk].speed == 5.0f);
    REQUIRE(golem.moves()[*walk].colnode.empty()); // a tab in the file
    REQUIRE(golem.moves()[*walk].target.allows(8.0f, 0.0f, 0.0f));
    REQUIRE_FALSE(golem.moves()[*walk].target.allows(6.0f, 0.0f, 0.0f));
    REQUIRE_FALSE(golem.moves()[*walk].target.allows(8.0f, kPi, 0.0f)); // behind it
    // The turn's cone points behind: a player behind it is turned to.
    const auto turn = golem.moveNamed("TURN");
    REQUIRE(turn.has_value());
    REQUIRE(golem.moves()[*turn].target.allows(8.0f, kPi, 0.0f));
    REQUIRE_FALSE(golem.moves()[*turn].target.allows(8.0f, 0.0f, 0.0f));
    const auto swing = golem.moveNamed("ATTACK1L");
    REQUIRE(swing.has_value());
    REQUIRE(golem.moves()[*swing].attack());
    REQUIRE(golem.moves()[*swing].harms());
    REQUIRE(golem.moves()[*swing].colnode == "BALL");
    REQUIRE(golem.damage(golem.moves()[*swing].damage0)->damage == 10.0f);
    REQUIRE(golem.moveOfType(CritterMove::kDeath).has_value());
    REQUIRE(golem.moveOfType(CritterMove::kRoar).has_value());
    REQUIRE_FALSE(golem.damage(99));
    // Its sounds and effects: the walk's two steps, the swing's swish at its sixth frame,
    // the stomp's ring where the heel lands, and the marks of a blow and a missile on it.
    REQUIRE(golem.sounds().size() == 11);
    REQUIRE(golem.moves()[*walk].sound == 6);
    REQUIRE(golem.moves()[*walk].soundFrame == 10);
    REQUIRE(golem.moves()[*walk].sound2 == 7);
    REQUIRE(golem.sound(6)->soundFor('G') == "S_GENGSTEP1");
    REQUIRE_FALSE(golem.sound(6)->shows());
    REQUIRE(golem.moves()[*swing].sound == 9);
    REQUIRE(golem.moves()[*swing].soundFrame == 6);
    REQUIRE(golem.sound(9)->soundFor('A') == "S_GOLASWING");
    const CritterSound* stomp = golem.sound(10);
    REQUIRE(stomp != nullptr);
    REQUIRE(stomp->tree == "EXPRING");
    REQUIRE(stomp->shows());
    REQUIRE(stomp->offset == Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE((stomp->flags & CritterSound::kShakes) != 0);
    REQUIRE(golem.damage(3)->sound == 10);
    REQUIRE_FALSE(golem.sound(8)->shows()); // NULLFX
    REQUIRE(golem.hitSoundClose() == 0);
    REQUIRE(golem.hitSoundFar() == 1);
    REQUIRE(golem.sound(golem.hitSoundClose())->tree == "HITDIE");
    REQUIRE(golem.sound(golem.hitSoundFar())->soundFor('G') == "S_GOLGHITFAR");
    REQUIRE(golem.sound(99) == nullptr);
    CritterData general;
    REQUIRE(general.load(root / "critter/GENERAL.json"));
    REQUIRE(general.kind() == kGeneralCritter);
    REQUIRE(general.tree() == "GENERAL1");
    REQUIRE(general.sight().maxDistance == 30.0f);
    CritterData missing;
    REQUIRE_FALSE(missing.load(root / "critter/NOBODY.json"));
    REQUIRE_FALSE(missing.loaded());
}

TEST_CASE("the dragon's animated root sits above its floor anchor, including hit effects",
          "[game][enemies][unpacked][assets]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
    const auto wad = test::assetOrSkip("CRITTER/DRAGON.WAD");
    const auto original = formats::parseCritterWad(readFile(wad));
    REQUIRE_FALSE(original.types.empty());
    REQUIRE(original.types.front().floorOffset == 18.5f);

    test::FakeRenderDevice device;
    WorldCollision collision;
    const auto ground = floor();
    collision.build(ground);
    Critters critters;
    critters.open(device, root, &collision, EnemyScales{}, 'B');
    const auto id = critters.spawn(kBossCritter, Vec3{0.0f, 2.0f, 0.0f}, 0.0f, "DRAGON");
    REQUIRE(id.has_value());
    REQUIRE(critters.positionOf(*id) == Vec3{0.0f}); // collision continues to use the floor
    const CritterData* data = critters.dataOf(*id);
    REQUIRE(data != nullptr);
    REQUIRE(data->floorOffset() == original.types.front().floorOffset);
    auto* archive = critters.archiveOf(*id);
    REQUIRE(archive != nullptr);
    const auto treeIndex = archive->trees.find(data->tree());
    REQUIRE(treeIndex.has_value());
    const auto& tree = archive->trees.tree(*treeIndex);
    const auto start = data->moveOfType(CritterMove::kStart);
    REQUIRE(start.has_value());
    const auto sequence = tree.findSequence(data->moves()[*start].anim);
    REQUIRE(sequence.has_value());
    TreePose pose;
    pose.evaluate(tree, *sequence, 0.0f);
    TreeModel model;
    REQUIRE(model.bind(tree, archive->models, archive->textures, device));
    model.setFrame(*sequence, 0);
    model.draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, pose.matrices());
    const auto local = device.draws;
    REQUIRE_FALSE(local.empty());
    device.draws.clear();
    critters.draw(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.size() == local.size());
    f32 error = 0.0f;
    for (usize draw = 0; draw < local.size(); ++draw) {
        REQUIRE(device.draws[draw].vertices.size() == local[draw].vertices.size());
        for (usize vertex = 0; vertex < local[draw].vertices.size(); ++vertex) {
            const Vec3 expected = local[draw].vertices[vertex].position + Vec3{0.0f, 18.5f, 0.0f};
            error = std::max(error,
                             glm::length(device.draws[draw].vertices[vertex].position - expected));
        }
    }
    REQUIRE(error < 0.0001f);
    EnemyHit hit;
    hit.damage = 20.0f;
    critters.hurt(*id, hit);
    const auto cues = critters.takeCues();
    REQUIRE_FALSE(cues.empty());
    const auto* sound = data->sound(data->hitSoundFar());
    REQUIRE(sound != nullptr);
    REQUIRE(cues.front().position.y == Approx(18.5f + data->originOffset().y + sound->offset.y));
}

TEST_CASE("the dragon wears its ice texture while frozen and restores its skin when thawing",
          "[game][enemies][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
    test::FakeRenderDevice device;
    Critters critters;
    critters.open(device, root, nullptr, EnemyScales{}, 'B');
    const auto id = critters.spawn(kBossCritter, Vec3{0.0f}, 0.0f, "DRAGON");
    REQUIRE(id.has_value());
    test::FakeTexture ice{1, 1};
    critters.freeze(*id, 1200);
    const auto checkSkin = [&](bool frozenSkin) {
        device.draws.clear();
        critters.draw(device, Mat4{1.0f}, {}, &ice);
        REQUIRE_FALSE(device.draws.empty());
        if (frozenSkin) {
            REQUIRE(std::ranges::any_of(device.draws, [](const auto& draw) {
                return draw.state.blend == BlendMode::Opaque;
            }));
        }
        for (const auto& draw : device.draws) {
            REQUIRE(draw.texture != &ice);
            REQUIRE((draw.state.maskedTexture == &ice) == frozenSkin);
            if (frozenSkin && draw.state.alphaTest == 0.0f) {
                REQUIRE(draw.state.blend == BlendMode::Opaque);
            }
        }
    };
    checkSkin(true);
    // Appearance belongs to this draw, not to simulation state or the shared model.
    device.draws.clear();
    critters.draw(device, Mat4{1.0f}, {});
    REQUIRE(critters.frozen(*id));
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        REQUIRE(draw.state.maskedTexture == nullptr);
    }
    checkSkin(true);
    critters.update(1040, 1040.0f / 60.0f, {}); // 160 remain: bit 3 clear
    checkSkin(true);
    critters.update(8, 8.0f / 60.0f, {}); // 152 remain: bit 3 set
    checkSkin(false);
    critters.update(8, 8.0f / 60.0f, {}); // 144 remain: bit 3 clear
    checkSkin(true);
    critters.update(144, 144.0f / 60.0f, {});
    REQUIRE_FALSE(critters.frozen(*id));
    checkSkin(false);
}

TEST_CASE("a boss's death record throws its coins all round it, up at seventy degrees",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    const std::filesystem::path wad = test::assetOrSkip("CRITTER/LICH.WAD");
    const formats::CritterFile file = formats::parseCritterWad(readFile(wad));
    REQUIRE(file.damages.size() > 1);
    REQUIRE(file.damages[1].type == 9);
    REQUIRE(file.damages[1].minSpeed == 30.0f);
    REQUIRE(file.damages[1].maxSpeed == 30.0f);
    CritterData lich;
    REQUIRE(lich.load(root / "critter/LICH.json"));
    const auto death = lich.moveOfType(CritterMove::kDeath);
    REQUIRE(death.has_value());
    REQUIRE(lich.moves()[*death].frameStart == 95);
    const CritterDamage* spew = lich.damage(lich.moves()[*death].damage0);
    REQUIRE(spew != nullptr);
    REQUIRE(spew->type == CritterDamage::kSpew);
    if (spew->speed == 0.0f) {
        SKIP("the critter data was unpacked before the spew's speed was read");
    }
    REQUIRE(spew->speed == 30.0f);
    REQUIRE(spew->spewHalfAngle() == Approx(kPi)); // all round
    const Vec3 way = spew->spewVelocity(0.0f);
    REQUIRE(glm::length(way) == Approx(30.0f));
    REQUIRE(way.x == Approx(0.0f).margin(0.001f));
    REQUIRE(way.z == Approx(30.0f * std::cos(1.2217305f)));
    REQUIRE(way.y == Approx(30.0f * std::sin(1.2217305f)));
    // Facing the other way, it throws the other way.
    REQUIRE(spew->spewVelocity(kPi).z == Approx(-way.z));
}

TEST_CASE("a golem walks up to the player it sees, strikes when in reach, and is worth its "
          "value in experience as it is worn down and killed",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("MONSTERS/GOLEM/LEVELG/animations.json");
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(floor());
    Critters critters;
    EnemyScales scales;
    scales.health = 0.75f;
    critters.open(device, root, &collision, scales, 'G');
    const auto id = critters.spawn(kGolemCritter, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(id.has_value());
    REQUIRE(critters.count() == 1);
    REQUIRE(critters.alive(*id));
    REQUIRE(critters.kindOf(*id) == kGolemCritter);
    REQUIRE(critters.maxHealthOf(*id) == 300.0f);
    REQUIRE(critters.healthOf(*id) == 300.0f);
    REQUIRE(critters.radiusOf(*id) == 4.0f);
    REQUIRE(critters.moveOf(*id) == "START");
    REQUIRE(critters.dataOf(*id) != nullptr);
    // Nobody about: the entrance plays out into the stance.
    const std::vector<EnemyView> nobody;
    for (int i = 0; i < 300; ++i) {
        critters.update(kTicks, kStep, nobody);
    }
    REQUIRE(critters.moveOf(*id) == "READY");
    REQUIRE(critters.positionOf(*id) == Vec3{0.0f, 0.0f, 0.0f});
    // A player twenty-five off, behind it: it turns and walks at them at five a second.
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, -25.0f})};
    for (int i = 0; i < 600 && critters.moveOf(*id) != "WALK"; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.moveOf(*id) == "WALK");
    REQUIRE(critters.targetOf(*id) == 0);
    const Vec3 from = critters.positionOf(*id);
    for (int i = 0; i < 30; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.positionOf(*id).z < from.z - 3.0f);
    REQUIRE(critters.positionOf(*id).z > from.z - 6.0f);
    REQUIRE(std::abs(critters.yawOf(*id)) > 3.0f); // facing -z
    // Within seven it attacks, and the blows land on the player.
    std::vector<CritterBlow> blows;
    bool attacked = false;
    for (int i = 0; i < 1200 && blows.empty(); ++i) {
        critters.update(kTicks, kStep, party);
        attacked = attacked || critters.moveOf(*id).starts_with("ATTACK");
        auto taken = critters.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE(attacked);
    REQUIRE_FALSE(blows.empty());
    REQUIRE(blows[0].player == 0);
    REQUIRE(blows[0].critter == *id);
    REQUIRE(blows[0].damage >= 10.0f);
    REQUIRE(blows[0].direction.z < 0.0f);
    REQUIRE(critters.positionOf(*id).z > -25.0f + 4.0f); // never onto the player
    // Its walk sounded its steps as it came (the fields' own, by the realm's letter), and
    // its attack its swish, or its stomp's ring where the heel came down; each once a move.
    const std::vector<CritterCue> cues = critters.takeCues();
    usize steps = 0;
    usize swishes = 0;
    usize rings = 0;
    for (const CritterCue& cue : cues) {
        REQUIRE(cue.critter == *id);
        steps += cue.sound == "S_GENGSTEP1" || cue.sound == "S_GENGSTEP2" ? 1U : 0U;
        swishes += cue.sound == "S_GOLGSWING" ? 1U : 0U;
        if (cue.sound == "S_GOLGSTOMP") {
            ++rings;
            REQUIRE(cue.tree == "EXPRING");
            REQUIRE(cue.shakes);
        }
    }
    REQUIRE(steps >= 2);
    REQUIRE(swishes + rings >= 1);
    // A hit takes its armour off and is worth its share of the value to the hitter; enough of
    // them make it roar. Where it lands, its own mark of a hit: a blow's close, a missile's
    // far.
    EnemyHit hit;
    hit.damage = 20.0f;
    hit.player = 0;
    hit.direction = Vec3{0.0f, 0.0f, 1.0f};
    hit.where = critters.positionOf(*id) + Vec3{1.0f, 5.0f, -2.0f};
    critters.hurt(*id, hit);
    REQUIRE(critters.healthOf(*id) == Approx(300.0f - 17.0f));
    auto marks = critters.takeCues();
    REQUIRE(marks.size() == 1);
    REQUIRE(marks[0].tree == "HITDIE");
    REQUIRE(marks[0].sound == "S_GOLGHITFAR");
    REQUIRE(marks[0].position == *hit.where);
    REQUIRE_FALSE(marks[0].follows);
    auto losses = critters.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].player == 0);
    REQUIRE_FALSE(losses[0].killed);
    REQUIRE(losses[0].experience == Approx(17.0f / 301.0f * 250.0f));
    hit.close = true;
    hit.where.reset();
    for (int i = 0; i < 3; ++i) {
        critters.hurt(*id, hit);
    }
    marks = critters.takeCues();
    REQUIRE(marks.size() == 3);
    REQUIRE(marks[0].sound == "S_GOLGHITCLOSE");
    REQUIRE(marks[0].position == critters.positionOf(*id) + critters.dataOf(*id)->originOffset());
    hit.close = false;
    bool roared = false;
    for (int i = 0; i < 60; ++i) {
        critters.update(kTicks, kStep, party);
        roared = roared || critters.moveOf(*id) == "ROAR";
    }
    REQUIRE(roared);
    // Thrown down, it gets up; a character under the place's level is paid less.
    EnemyHit knock = hit;
    knock.flags = EnemyHit::kKnockDown;
    critters.hurt(*id, knock);
    critters.update(kTicks, kStep, party);
    REQUIRE(critters.moveOf(*id) == "KD");
    Critters seasoned;
    EnemyScales place;
    place.playerLevel = 20.0f;
    seasoned.open(device, root, &collision, place, 'G');
    const auto other = seasoned.spawn(kGolemCritter, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(other.has_value());
    EnemyHit weak = hit;
    weak.level = 10;
    seasoned.hurt(*other, weak);
    REQUIRE(seasoned.takeLosses()[0].experience == Approx(17.0f / 401.0f * 250.0f * 0.8f));
    // The killing blow: a fifth of its value to everyone, its death played out, then gone.
    EnemyHit slay;
    slay.damage = 1000.0f;
    slay.player = 0;
    critters.takeLosses();
    critters.hurt(*id, slay);
    losses = critters.takeLosses();
    REQUIRE(losses.size() == 2);
    REQUIRE(losses[1].killed);
    REQUIRE(losses[1].player == -1);
    REQUIRE(losses[1].experience == 50.0f);
    REQUIRE(critters.dying(*id));
    REQUIRE_FALSE(critters.alive(*id));
    REQUIRE(critters.targets().empty());
    critters.update(kTicks, kStep, party);
    REQUIRE(critters.moveOf(*id) == "DEATH");
    int gone = 0;
    while (critters.count() > 0 && gone < 600) {
        critters.update(kTicks, kStep, party);
        ++gone;
    }
    REQUIRE(critters.count() == 0);
    REQUIRE(gone > 30);
}

TEST_CASE("a general comes with the realm's costume and is found by missiles and sweeps",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("MONSTERS/GENERAL/LEVELG/animations.json");
    test::FakeRenderDevice device;
    Critters critters;
    critters.open(device, root, nullptr, EnemyScales{}, 'G');
    const auto general = critters.spawn(kGeneralCritter, Vec3{10.0f, 0.0f, 0.0f}, kPi / 2.0f);
    REQUIRE(general.has_value());
    REQUIRE(critters.kindOf(*general) == kGeneralCritter);
    REQUIRE(critters.maxHealthOf(*general) == 200.0f);
    REQUIRE(critters.radiusOf(*general) == 3.5f);
    REQUIRE(critters.targets().size() == 1);
    REQUIRE(critters.targets()[0].radius == 3.5f);
    REQUIRE((critters.struckBy(Vec3{-5.0f, 4.0f, 0.0f}, Vec3{30.0f, 4.0f, 0.0f}, 0.5f) == general));
    REQUIRE_FALSE(critters.struckBy(Vec3{-5.0f, 4.0f, 10.0f}, Vec3{30.0f, 4.0f, 10.0f}, 0.5f).has_value());
    REQUIRE(critters.within(Vec3{10.0f, 4.0f, 0.0f}, 1.0f).size() == 1);
    REQUIRE(critters.reachedBy(Vec3{0.0f, 0.0f, 0.0f}, 12.0f, 0.5f, Vec3{1.0f, 0.0f, 0.0f}).size() == 1);
    REQUIRE(critters.reachedBy(Vec3{0.0f, 0.0f, 0.0f}, 12.0f, 0.5f, Vec3{-1.0f, 0.0f, 0.0f}).empty());
    // A gargoyle comes by its form, and falling is worth the key named by it.
    test::unpackedOrSkip("MONSTERS/GAR_EAGL/animations.json");
    const auto gargoyle = critters.spawn(kGargoyleCritter, Vec3{-20.0f, 0.0f, 0.0f}, 0.0f, "GAR_EAGL");
    REQUIRE(gargoyle.has_value());
    REQUIRE(critters.formOf(*gargoyle) == "EAGL");
    REQUIRE(critters.formOf(*general).empty());
    REQUIRE(critters.maxHealthOf(*gargoyle) == 600.0f);
    EnemyHit slay;
    slay.damage = 2000.0f;
    slay.player = 0;
    critters.hurt(*gargoyle, slay);
    const auto losses = critters.takeLosses();
    REQUIRE(losses.size() == 2);
    REQUIRE(losses[1].killed);
    REQUIRE(losses[1].form == "EAGL");
    REQUIRE(losses[1].experience == 100.0f);
    // A kind without data, or a form without an archive, is refused.
    REQUIRE_FALSE(critters.spawn(99, Vec3{0.0f, 0.0f, 0.0f}, 0.0f).has_value());
    REQUIRE_FALSE(critters.spawn(kGargoyleCritter, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, "GAR_NONE").has_value());
}

TEST_CASE("a critter held keeps its stance, roars when asked, stands frozen, loses its "
          "targets blinded, and is curbed of its flagged attacks",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = unpackedRoot();
    test::unpackedOrSkip("MONSTERS/GOLEM/LEVELG/animations.json");
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(floor());
    Critters critters;
    critters.open(device, root, &collision, EnemyScales{}, 'G');
    const auto id = critters.spawn(kGolemCritter, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(id.has_value());
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, -25.0f})};
    // Held, it never leaves its stance for the player it sees.
    critters.hold(*id, true);
    for (int i = 0; i < 600; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.moveOf(*id) == "READY");
    REQUIRE(critters.moveTypeOf(*id) == CritterMove::kReady);
    REQUIRE(critters.targetOf(*id) == 0);
    REQUIRE(critters.positionOf(*id) == Vec3{0.0f, 0.0f, 0.0f});
    // Asked to roar, it does as soon as its stance is over, and then holds again.
    critters.roar(*id);
    bool roared = false;
    for (int i = 0; i < 600 && !roared; ++i) {
        critters.update(kTicks, kStep, party);
        roared = critters.moveTypeOf(*id) == CritterMove::kRoar;
    }
    REQUIRE(roared);
    for (int i = 0; i < 600; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.moveOf(*id) == "READY");
    // Let go, it comes; frozen, it stops as it is, animation and all, and is not gone
    // after the freeze.
    critters.hold(*id, false);
    for (int i = 0; i < 600 && critters.moveOf(*id) != "WALK"; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.moveOf(*id) == "WALK");
    const Vec3 stopped = critters.positionOf(*id);
    critters.freeze(*id, 120);
    REQUIRE(critters.frozen(*id));
    for (int i = 0; i < 30; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.frozen(*id));
    REQUIRE(critters.positionOf(*id) == stopped);
    for (int i = 0; i < 40; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE_FALSE(critters.frozen(*id));
    for (int i = 0; i < 30; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE(critters.positionOf(*id) != stopped);
    // Blinded, it finds no one and turns at a tenth; then it sees again.
    critters.blind(*id, 60);
    REQUIRE(critters.blinded(*id));
    critters.update(kTicks, kStep, party);
    REQUIRE(critters.targetOf(*id) == -1);
    for (int i = 0; i < 40; ++i) {
        critters.update(kTicks, kStep, party);
    }
    REQUIRE_FALSE(critters.blinded(*id));
    critters.update(kTicks, kStep, party);
    REQUIRE(critters.targetOf(*id) == 0);
    // A curb refuses the attacks whose harm is flagged for it, until lifted; and the
    // figure can be stood at another size.
    critters.curb(*id, 0.5f);
    REQUIRE(critters.curbed(*id));
    critters.curb(*id, 0.0f);
    REQUIRE_FALSE(critters.curbed(*id));
    REQUIRE(critters.scaleOf(*id) == 1.0f);
    critters.resize(*id, 0.8f);
    REQUIRE(critters.scaleOf(*id) == 0.8f);
    critters.resize(*id, 0.0f);
    REQUIRE(critters.scaleOf(*id) == 0.8f);
    // None of it touches a slot that holds nothing.
    critters.freeze(5, 10);
    critters.hold(5, true);
    REQUIRE_FALSE(critters.frozen(5));
}

} // namespace
