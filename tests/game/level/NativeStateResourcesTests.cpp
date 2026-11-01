#include <array>
#include <functional>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/ParticleField.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "formats/CritterWad.h"
#include "game/critters/CritterData.h"
#include "game/level/ItemFigure.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    std::filesystem::path directory;
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterData critter;

    explicit Fixture(std::string_view name) : directory(test::scratchDirectory(name)) {
        writeTextFile(directory / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeFile(directory / "white.png", test::kTinyPng);
        writeTextFile(directory / "objects.json", R"({"objects":[
          {"name":"ROCKL1","file":"tri.obj"},{"name":"OTHER","file":"tri.obj"}]})");
        writeTextFile(directory / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"SECOND","file":"white.png","width":2,"height":2}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"ACTOR","nodes":[{"name":"ROOT","object":"ROCKL1","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":10,"frameRate":30},{"name":"MOVE","frames":15,"frameRate":30}]},
          {"name":"EFFECT","nodes":[{"name":"ROOT","object":"OTHER","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":10,"frameRate":30}]}]})");
        writeTextFile(directory / "critter.json", R"({"name":"TEST","types":[{"moveCount":1}],
          "descriptors":[{}],"moves":[{}],"damages":[{"type":1,"damage":12}],"sounds":[]})");
        test::convertModelFixture(directory);
        REQUIRE(archive.load(directory));
        REQUIRE(critter.load(directory / "critter.json"));
        REQUIRE(archive.trees.size() == 2);
        REQUIRE(archive.textures.size() == 2);
    }
};

template <class T>
std::optional<NativeStateDigest::Digest> resourceHash(const NativeStateResources& registry,
                                                      const T* resource) {
    NativeStateDigest digest(registry.resources());
    digest.resource(resource);
    return digest.checkedFinish();
}

NativeStateDigest::Digest figureHash(const NativeStateResources& registry,
                                     const ItemFigure& figure) {
    NativeStateDigest digest(registry.resources());
    digest.value(figure);
    REQUIRE(digest.checkedFinish());
    return *digest.checkedFinish();
}

TEST_CASE("native critter resource identity includes decoded impact policies",
          "[native-digest][native-state-resources][native-impact-envelope]") {
    formats::CritterFile source;
    source.descriptors.resize(1);
    source.types.resize(1);
    source.types[0].moveCount = 1;
    source.moves.resize(1);
    source.sounds.resize(1);
    auto& effect = source.sounds[0];
    effect.name = "EXPLOSION";
    effect.impactRadius = 3.6f;
    effect.impactPhysicalFraction = 0.1f;
    effect.impactElementalFraction = 0.9f;
    effect.impactElement = 4;
    CritterRules rules;
    rules.nativeImpactAreas = true;
    const auto original = CritterData::fromDecoded("TEST", source, 0, rules);
    const auto identical = CritterData::fromDecoded("TEST", source, 0, rules);
    NativeStateResources first;
    NativeStateResources second;
    REQUIRE(first.registerCritter(original, "native/TEST", 0));
    REQUIRE(second.registerCritter(identical, "native/TEST", 0));
    const auto originalIdentity = resourceHash(first, &original);
    const auto originalEffect = resourceHash(first, original.sound(0));
    REQUIRE(originalIdentity);
    REQUIRE(originalEffect);
    CHECK(originalIdentity == resourceHash(second, &identical));
    CHECK(originalEffect == resourceHash(second, identical.sound(0)));
    for (u32 field = 0; field < 5; ++field) {
        CAPTURE(field);
        auto changed = source;
        auto& sound = changed.sounds[0];
        switch (field) {
        case 0: sound.impactRadius = 5; break;
        case 1: sound.impactPhysicalFraction = 0.2f; break;
        case 2: sound.impactElementalFraction = 0.8f; break;
        case 3: sound.impactElement = 2; break;
        case 4: sound.scale = 0.72f; break;
        }
        const auto altered = CritterData::fromDecoded("TEST", changed, 0, rules);
        NativeStateResources registry;
        REQUIRE(registry.registerCritter(altered, "native/TEST", 0));
        const auto alteredIdentity = resourceHash(registry, &altered);
        const auto alteredEffect = resourceHash(registry, altered.sound(0));
        REQUIRE(alteredIdentity);
        REQUIRE(alteredEffect);
        CHECK(originalIdentity != alteredIdentity);
        CHECK(originalEffect != alteredEffect);
    }
}

TEST_CASE("native builtin white textures identify particle fallbacks without renderer writes",
          "[native-digest][native-state-resources][native-builtin-texture]") {
    const test::FakeRenderDevice first{};
    const test::FakeRenderDevice second{};
    NativeStateResources a;
    NativeStateResources b;
    const Texture& whiteA = first.whiteTexture();
    const Texture& whiteB = second.whiteTexture();
    REQUIRE(&whiteA != &whiteB);
    CHECK_FALSE(resourceHash(a, &whiteA));
    CHECK_FALSE(resourceHash(b, &whiteB));

    ParticleDescriptor descriptor;
    descriptor.oneShot = true;
    descriptor.maxParticles = 2;
    descriptor.particleLife = 30;
    descriptor.width = {1, 1, 1, 1};
    ParticleField left;
    ParticleField right;
    left.start(descriptor, Mat4{1}, &whiteA, 17);
    right.start(descriptor, Mat4{1}, &whiteB, 17);
    const auto hash = [](const NativeStateResources& registry, const ParticleField& field) {
        NativeStateDigest digest(registry.resources());
        digest.value(field);
        return digest.checkedFinish();
    };
    CHECK_FALSE(hash(a, left));
    CHECK_FALSE(hash(b, right));
    REQUIRE(a.registerBuiltinTexture(whiteA, "opaque-white"));
    REQUIRE(b.registerBuiltinTexture(whiteB, "opaque-white"));
    CHECK(resourceHash(a, &whiteA) == resourceHash(b, &whiteB));
    CHECK_FALSE(resourceHash(a, &whiteB)); // Registration is explicit, not a type-wide fallback.
    for (u32 tick = 0; tick < 8; ++tick) {
        left.step(1.0f / 30);
        right.step(1.0f / 30);
        REQUIRE(left.particleCount() == 2);
        REQUIRE(right.particleCount() == 2);
        REQUIRE(hash(a, left));
        CHECK(hash(a, left) == hash(b, right));
        REQUIRE(a.registerBuiltinTexture(whiteA, "opaque-white"));
    }

    NativeStateResources otherRole;
    REQUIRE(otherRole.registerBuiltinTexture(whiteB, "transparent-black"));
    CHECK(resourceHash(a, &whiteA) != resourceHash(otherRole, &whiteB));
    CHECK_FALSE(a.registerBuiltinTexture(whiteA, "transparent-black"));
    CHECK_FALSE(a.good());
    CHECK_FALSE(hash(a, left));
    a.clear();
    CHECK_FALSE(a.registerBuiltinTexture(whiteA, ""));
    CHECK_FALSE(a.good());
    CHECK(first.texturesCreated == 0);
    CHECK(second.texturesCreated == 0);
    CHECK(first.textureUpdates == 0);
    CHECK(second.textureUpdates == 0);
    CHECK(first.frames == 0);
    CHECK(second.frames == 0);
    CHECK(first.draws.empty());
    CHECK(second.draws.empty());
}

TEST_CASE("native resource identities are independent of allocation and texture load order",
          "[native-digest][native-state-resources]") {
    Fixture first("native-resources-first");
    Fixture second("native-resources-second");
    NativeStateResources a;
    NativeStateResources b;
    REQUIRE(a.registerArchive(first.archive, "realm/G/items"));
    REQUIRE(b.registerArchive(second.archive, "realm/G/items"));
    CHECK(first.device.texturesCreated == 0);
    CHECK(second.device.texturesCreated == 0);
    CHECK(first.archive.textures.cachedTexture(0) == nullptr);
    CHECK(first.archive.textures.cachedTexture(50) == nullptr);
    REQUIRE(resourceHash(a, &first.archive));
    CHECK(resourceHash(a, &first.archive) == resourceHash(b, &second.archive));
    REQUIRE(resourceHash(a, &first.archive.textures));
    CHECK(resourceHash(a, &first.archive.textures) == resourceHash(b, &second.archive.textures));
    for (u32 tree = 0; tree < 2; ++tree) {
        const auto& left = first.archive.trees.tree(tree);
        const auto& right = second.archive.trees.tree(tree);
        REQUIRE(&left != &right);
        REQUIRE(resourceHash(a, &left));
        CHECK(resourceHash(a, &left) == resourceHash(b, &right));
        for (usize sequence = 0; sequence < left.sequences.size(); ++sequence) {
            CHECK(resourceHash(a, &left.sequences[sequence]) ==
                  resourceHash(b, &right.sequences[sequence]));
        }
    }
    CHECK(resourceHash(a, &first.archive.trees.tree(0)) !=
          resourceHash(a, &first.archive.trees.tree(1)));
    CHECK(resourceHash(a, first.archive.trees.tree(0).sequences.data()) !=
          resourceHash(a, &first.archive.trees.tree(0).sequences[1]));

    const auto& left0 = first.archive.textures.texture(first.device, 0);
    const auto& right1 = second.archive.textures.texture(second.device, 1);
    CHECK_FALSE(resourceHash(a, &left0));
    REQUIRE(a.refreshArchive(first.archive));
    REQUIRE(b.registerArchive(second.archive, "realm/G/items"));
    const auto& left1 = first.archive.textures.texture(first.device, 1);
    const auto& right0 = second.archive.textures.texture(second.device, 0);
    CHECK_FALSE(resourceHash(a, &left1));
    REQUIRE(a.refreshArchive(first.archive));
    REQUIRE(b.refreshArchive(second.archive));
    CHECK(resourceHash(a, &left0) == resourceHash(b, &right0));
    CHECK(resourceHash(a, &left1) == resourceHash(b, &right1));
    CHECK(resourceHash(a, &left0) != resourceHash(a, &left1));
    for (u32 repeat = 0; repeat < 8; ++repeat) {
        REQUIRE(a.registerArchive(first.archive, "realm/G/items"));
        REQUIRE(b.refreshArchive(second.archive));
        REQUIRE(resourceHash(a, &left0));
    }
    CHECK(first.device.texturesCreated == 2);
    CHECK(second.device.texturesCreated == 2);
    CHECK(first.device.textureUpdates == 0);
    CHECK(second.device.textureUpdates == 0);

    // An external stage texture bank does not need a synthetic ItemArchive.
    TextureSet worldA;
    TextureSet worldB;
    REQUIRE(worldA.load(first.directory));
    REQUIRE(worldB.load(second.directory));
    const auto& world0 = worldA.texture(first.device, 0);
    const auto& world1 = worldB.texture(second.device, 0);
    REQUIRE(a.registerTextures(worldA, "stage/G2/world"));
    REQUIRE(b.registerTextures(worldB, "stage/G2/world"));
    REQUIRE(resourceHash(a, &worldA));
    CHECK(resourceHash(a, &worldA) == resourceHash(b, &worldB));
    CHECK(resourceHash(a, &worldA) != resourceHash(a, &first.archive.textures));
    const auto bankBefore = resourceHash(a, &worldA);
    const auto& worldSecond = worldA.texture(first.device, 1);
    CHECK_FALSE(resourceHash(a, &worldSecond));
    REQUIRE(a.refreshTextures(worldA));
    CHECK(resourceHash(a, &worldSecond));
    CHECK(resourceHash(a, &worldA) == bankBefore);
    CHECK(resourceHash(a, &world0) == resourceHash(b, &world1));
    CHECK(resourceHash(a, &world0) != resourceHash(a, &left0));
}

TEST_CASE("native resource lookup fails closed for unknown types and conflicting registrations",
          "[native-digest][native-state-resources]") {
    Fixture fixture("native-resources-guards");
    NativeStateResources registry;
    REQUIRE(registry.registerArchive(fixture.archive, "realm/G/items"));
    REQUIRE(resourceHash(registry, &fixture.archive));
    CHECK_FALSE(resourceHash(registry, static_cast<const void*>(&fixture.archive)));
    const s32 unsupported = 5;
    CHECK_FALSE(resourceHash(registry, &unsupported));
    TreeInfo foreign;
    foreign.name = fixture.archive.trees.tree(0).name;
    CHECK_FALSE(resourceHash(registry, &foreign));
    CHECK(resourceHash(registry, static_cast<const TreeInfo*>(nullptr)));
    CHECK(registry.good()); // Unknown queries invalidate their sink, not other users.

    CHECK_FALSE(registry.registerArchive(fixture.archive, "realm/B/items"));
    CHECK_FALSE(registry.good());
    CHECK_FALSE(resourceHash(registry, &fixture.archive));
    registry.clear();
    CHECK_FALSE(resourceHash(registry, &fixture.archive));
    REQUIRE(registry.registerArchive(fixture.archive, "realm/G/items"));
    const auto& texture = fixture.archive.textures.texture(fixture.device, 0);
    REQUIRE(registry.registerTexture(texture, fixture.archive, 0));
    CHECK_FALSE(registry.registerTexture(texture, fixture.archive, 1));
    CHECK_FALSE(registry.good());
    registry.clear();
    const TreeModel model;
    CHECK_FALSE(registry.registerModel(model, fixture.archive, "shot/0"));
    CHECK_FALSE(registry.good());
    registry.clear();
    CHECK_FALSE(registry.refreshTextures(fixture.archive.textures));
    CHECK_FALSE(registry.good());
    registry.clear();
    REQUIRE(registry.registerArchive(fixture.archive, "realm/G/items"));
    CHECK_FALSE(registry.registerModel(model, fixture.archive, ""));
    CHECK_FALSE(registry.good());
    registry.clear();
    REQUIRE(registry.registerArchive(fixture.archive, "realm/G/items"));
    fixture.archive.release();
    CHECK_FALSE(registry.refreshArchive(fixture.archive));
    CHECK_FALSE(registry.good());
}

TEST_CASE("native resource roles distinguish model critter animation and collision bindings",
          "[native-digest][native-state-resources]") {
    Fixture first("native-resource-roles-a");
    Fixture second("native-resource-roles-b");
    NativeStateResources a;
    NativeStateResources b;
    TreeModel modelA;
    TreeModel modelB;
    const TreeModel anotherModel{};
    TextureAnimator animatorA;
    const TextureAnimator animatorB{};
    const WorldCollision collisionA{};
    const WorldCollision collisionB{};
    REQUIRE(modelA.bind(first.archive.trees.tree(0), first.archive.models, first.archive.textures,
                        first.device));
    REQUIRE(modelB.bind(second.archive.trees.tree(0), second.archive.models,
                        second.archive.textures, second.device));
    REQUIRE(a.registerArchive(first.archive, "enemy/zombie"));
    REQUIRE(b.registerArchive(second.archive, "enemy/zombie"));
    REQUIRE(a.registerModel(modelA, first.archive, "projectile/0"));
    REQUIRE(b.registerModel(modelB, second.archive, "projectile/0"));
    REQUIRE(a.registerModel(anotherModel, first.archive, "projectile/1"));
    CHECK(resourceHash(a, &modelA) == resourceHash(b, &modelB));
    CHECK(resourceHash(a, &modelA) != resourceHash(a, &anotherModel));
    REQUIRE(a.registerTextureAnimator(animatorA, first.archive, "generator/2/textures"));
    REQUIRE(b.registerTextureAnimator(animatorB, second.archive, "generator/2/textures"));
    CHECK(resourceHash(a, &animatorA) == resourceHash(b, &animatorB));
    animatorA.step(5);
    // The pointer identifies immutable binding, not its independently hashed clock.
    CHECK(resourceHash(a, &animatorA) == resourceHash(b, &animatorB));
    REQUIRE(a.registerCritter(first.critter, "CRITTER/GOLEM.WAD", 0));
    REQUIRE(b.registerCritter(second.critter, "CRITTER/GOLEM.WAD", 0));
    CHECK(resourceHash(a, &first.critter) == resourceHash(b, &second.critter));
    REQUIRE(a.registerCollision(collisionA, "stage/G2"));
    REQUIRE(b.registerCollision(collisionB, "stage/G2"));
    CHECK(resourceHash(a, &collisionA) == resourceHash(b, &collisionB));

    NativeStateResources otherType;
    REQUIRE(otherType.registerCritter(second.critter, "CRITTER/GOLEM.WAD", 1));
    CHECK(resourceHash(a, &first.critter) != resourceHash(otherType, &second.critter));
    CHECK_FALSE(a.registerModel(modelA, first.archive, "projectile/1"));
    CHECK_FALSE(a.good());
}

TEST_CASE("native standalone animation banks and empty lenders have stable typed identities",
          "[native-digest][native-state-resources]") {
    const Fixture first("native-actions-a");
    const Fixture second("native-actions-b");
    AnimationSet left;
    AnimationSet right;
    REQUIRE(left.load(first.directory));
    REQUIRE(right.load(second.directory));
    NativeStateResources a;
    NativeStateResources b;
    REQUIRE(a.registerAnimations(left, "players/kni/anim"));
    REQUIRE(b.registerAnimations(right, "players/kni/anim"));
    REQUIRE(resourceHash(a, &left));
    CHECK(resourceHash(a, &left) == resourceHash(b, &right));
    for (u32 i = 0; i < left.size(); ++i) {
        REQUIRE(resourceHash(a, &left.tree(i)));
        CHECK(resourceHash(a, &left.tree(i)) == resourceHash(b, &right.tree(i)));
        CHECK(resourceHash(a, &left.tree(i).sequences.front()) ==
              resourceHash(b, &right.tree(i).sequences.front()));
    }
    REQUIRE(a.registerAnimations(left, "players/kni/anim"));
    const TextureSet emptyA{};
    const TextureSet emptyB{};
    REQUIRE(a.registerTextures(emptyA, "stage/G1/empty-realm"));
    REQUIRE(b.registerTextures(emptyB, "stage/G1/empty-realm"));
    REQUIRE(resourceHash(a, &emptyA));
    CHECK(resourceHash(a, &emptyA) == resourceHash(b, &emptyB));
    REQUIRE(a.refreshTextures(emptyA));
    CHECK_FALSE(a.registerAnimations(left, "players/arc/anim"));
    CHECK_FALSE(resourceHash(a, &left));
    a.clear();
    CHECK_FALSE(resourceHash(a, &left));
    CHECK_FALSE(resourceHash(a, &emptyA));
    CHECK(first.device.texturesCreated == 0);
    CHECK(second.device.texturesCreated == 0);
}

TreeInfo generatedTree() {
    TreeInfo tree;
    tree.name = "ROCK";
    TreeNodeInfo node;
    node.name = "ROOT";
    node.object = "ROCKL1";
    node.objectFrames.push_back({"OTHER", 1, 3});
    tree.nodes.push_back(node);
    TreeSequenceInfo sequence;
    sequence.name = "ACTIVE";
    sequence.frames = 5;
    sequence.tracks.push_back({0, TrackInfo::kChannels, {0, 4}, {0, 1}});
    sequence.trackOfNode = {0};
    tree.sequences.push_back(sequence);
    return tree;
}

TEST_CASE("generated native tree identities include construction not just name or role",
          "[native-digest][native-state-resources]") {
    const Fixture fixture("native-resource-generated");
    NativeStateResources registry;
    REQUIRE(registry.registerArchive(fixture.archive, "realm/G/items"));
    const auto base = generatedTree();
    const auto duplicate = base;
    REQUIRE(registry.registerGeneratedTree(base, fixture.archive, "generator/1/state/0"));
    REQUIRE(registry.registerGeneratedTree(duplicate, fixture.archive, "generator/1/state/0"));
    REQUIRE(resourceHash(registry, &base));
    CHECK(resourceHash(registry, &base) == resourceHash(registry, &duplicate));
    CHECK(resourceHash(registry, base.sequences.data()) ==
          resourceHash(registry, duplicate.sequences.data()));
    const std::array<std::function<void(TreeInfo&)>, 15> mutations{
        [](auto& t) { t.prefix = "DIFFERENT"; },
        [](auto& t) { t.nodes[0].object = "OTHER"; },
        [](auto& t) { t.nodes[0].objectFlags = object_flags::kAdditive; },
        [](auto& t) { t.nodes[0].position.x = 3; },
        [](auto& t) { t.nodes[0].type = 4; },
        [](auto& t) { t.nodes[0].flags = 8; },
        [](auto& t) { t.nodes[0].direction.z = 1; },
        [](auto& t) { t.nodes[0].particle = 2; },
        [](auto& t) { t.nodes[0].textureAnimation = 1; },
        [](auto& t) { t.nodes[0].objectFrames[0].frames = 4; },
        [](auto& t) { t.sequences[0].frameRate = 15; },
        [](auto& t) { t.sequences[0].repeats = true; },
        [](auto& t) { t.sequences[0].flags = 1; },
        [](auto& t) { t.sequences[0].tracks[0].values[1] = 2; },
        [](auto& t) { t.sequences[0].trackOfNode[0] = -1; }};
    for (usize index = 0; index < mutations.size(); ++index) {
        CAPTURE(index);
        auto changed = base;
        mutations[index](changed);
        NativeStateResources alternative;
        REQUIRE(alternative.registerArchive(fixture.archive, "realm/G/items"));
        REQUIRE(alternative.registerGeneratedTree(changed, fixture.archive, "generator/1/state/0"));
        CHECK(resourceHash(registry, &base) != resourceHash(alternative, &changed));
        CHECK(resourceHash(registry, base.sequences.data()) !=
              resourceHash(alternative, changed.sequences.data()));
    }
}

TEST_CASE("static item figure resources retain discarded branches and resolve object suffixes",
          "[native-digest][native-state-resources][local-rewind]") {
    Fixture first("native-resource-static-a");
    Fixture second("native-resource-static-b");
    NativeStateResources a;
    NativeStateResources b;
    ItemFigure figureA;
    ItemFigure figureB;
    ItemInstance instance;
    instance.position = {2, 4, 7};
    REQUIRE(figureA.placeStaticFallback(first.device, first.archive, "ROCK", instance, nullptr, 0));
    REQUIRE(
        figureB.placeStaticFallback(second.device, second.archive, "ROCK", instance, nullptr, 0));
    REQUIRE(figureA.presentation().tree->nodes.front().object == "ROCKL1");
    const auto savedA = figureA;
    const auto savedB = figureB;
    REQUIRE(figureA.placeStaticFallback(first.device, first.archive, "ROCK", instance, nullptr,
                                        object_flags::kAdditive));
    REQUIRE(figureB.placeStaticFallback(second.device, second.archive, "ROCK", instance, nullptr,
                                        object_flags::kAdditive));
    REQUIRE(a.registerArchive(first.archive, "realm/G/items"));
    REQUIRE(b.registerArchive(second.archive, "realm/G/items"));
    // Register only the current owner: history's retained generated trees must
    // resolve without consulting/restoring that history or creating new artwork.
    REQUIRE(figureA.registerNativeResources(a));
    REQUIRE(figureB.registerNativeResources(b));
    CHECK(figureHash(a, savedA) == figureHash(b, savedB));
    CHECK(figureHash(a, figureA) == figureHash(b, figureB));
    CHECK(figureHash(a, savedA) != figureHash(a, figureA));
    const auto textures = first.device.texturesCreated;
    for (u32 tick = 0; tick < 8; ++tick) {
        figureA.update(1.0f / 60);
        figureB.update(1.0f / 60);
        CHECK(figureHash(a, figureA) == figureHash(b, figureB));
    }
    figureA = savedA;
    figureB = savedB;
    CHECK(figureHash(a, figureA) == figureHash(b, figureB));
    CHECK(first.device.texturesCreated == textures);
    CHECK(first.device.textureUpdates == 0);

    NativeStateResources unregistered;
    CHECK_FALSE(figureA.registerNativeResources(unregistered));
    CHECK_FALSE(unregistered.good());
}

TEST_CASE("native image bindings observe cached pixels without decoding or uploading",
          "[native-digest][native-state-resources][native-image]") {
    Fixture first("native-images-first");
    Fixture second("native-images-second");
    NativeStateResources a;
    NativeStateResources b;
    REQUIRE(a.registerArchive(first.archive, "test/items"));
    REQUIRE(b.registerArchive(second.archive, "test/items"));
    CHECK(first.archive.textures.cachedImage(0) == nullptr);
    const auto& left = first.archive.textures.image(0);
    const auto& right = second.archive.textures.image(0);
    CHECK_FALSE(resourceHash(a, &left));
    const auto textures = first.device.texturesCreated;
    REQUIRE(a.refreshArchive(first.archive));
    REQUIRE(b.refreshArchive(second.archive));
    REQUIRE(resourceHash(a, &left));
    CHECK(resourceHash(a, &left) == resourceHash(b, &right));
    CHECK(first.device.texturesCreated == textures);
    CHECK(first.archive.textures.cachedImage(1) == nullptr);
    CHECK(first.archive.textures.cachedTexture(0) == nullptr);

    const Image unknown = left;
    CHECK_FALSE(resourceHash(a, &unknown));
    REQUIRE(a.registerImage(unknown, "test/scroll"));
    const Image equal = unknown;
    REQUIRE(b.registerImage(equal, "test/scroll"));
    CHECK(resourceHash(a, &unknown) == resourceHash(b, &equal));
    Image different = unknown;
    different.pixels.front() ^= 1;
    NativeStateResources c;
    REQUIRE(c.registerImage(different, "test/scroll"));
    CHECK(resourceHash(a, &unknown) != resourceHash(c, &different));
    CHECK_FALSE(a.registerImage(unknown, "wrong-role"));
    a.clear();
    CHECK_FALSE(resourceHash(a, &unknown));
}

} // namespace
