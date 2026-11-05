#include <array>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/level/ItemFigure.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path figureArchive() {
    const auto root = test::sampleLevel("item-figure-local-history");
    writeTextFile(root / "animations.json", R"({
      "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":2}],
      "particles":[{"enables":16928,"texture":"STONE","particleLife":[0.2,0.1],
        "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"CHEST","nodes":[
        {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
        {"name":"NULL1","parent":0,"position":[0,1,0],"particle":0}],
        "sequences":[{"name":"OFF","frames":2,"frameRate":30},
          {"name":"ACTIVE","frames":8,"frameRate":30,
            "tracks":[{"node":1,"flags":32,"frames":[0,7],"values":[1,5]}]},
          {"name":"OPEN","frames":0}]},
        {"name":"RELIC","nodes":[
          {"name":"ROOT","object":"BLADE","parent":-1,"position":[0,0,0]}],
         "sequences":[{"name":"ACTIVE","frames":5,"frameRate":30,
           "tracks":[{"node":0,"flags":16,"frames":[0,4],"values":[0,3]}]}]}]})");
    return root;
}

/** The figure's native digest, its archive and the white texture under fixed keys, so two
 * figures that bound the same artwork apart still compare. */
NativeStateDigest::Digest registeredDigest(const ItemFigure& figure, const ItemArchive& archive,
                                           test::FakeRenderDevice& device) {
    NativeStateResources registry;
    REQUIRE(registry.registerArchive(archive, "stage/item-figure"));
    REQUIRE(registry.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    REQUIRE(figure.registerNativeResources(registry));
    NativeStateDigest digest(registry.resources());
    digest.value(figure);
    const auto result = digest.checkedFinish();
    REQUIRE(result.has_value());
    return *result;
}

void compare(const ItemFigure& actual, const ItemFigure& expected, const ItemArchive& archive,
             test::FakeRenderDevice& device) {
    CHECK(actual.hasFigure() == expected.hasFigure());
    CHECK(actual.sequence() == expected.sequence());
    CHECK(actual.position() == expected.position());
    CHECK(actual.transform() == expected.transform());
    CHECK(actual.progress() == expected.progress());
    CHECK(actual.finished() == expected.finished());
    CHECK(actual.nodeTransform("NULL1") == expected.nodeTransform("NULL1"));
    CHECK(actual.textureAnimator().clock() == expected.textureAnimator().clock());
    CHECK(actual.particles().checkpoint() == expected.particles().checkpoint());
    CAPTURE(actual.presentation().tree ? actual.presentation().tree->name : "");
    // The pose drawing leaves behind is scratch, not a simulation input; the digest omits it.
    CHECK(registeredDigest(actual, archive, device) == registeredDigest(expected, archive, device));
    CameraFrame camera;
    camera.right = {1, 0, 0};
    camera.up = {0, 1, 0};
    for (f32 const blend : {-1.0f, 0.0f, 0.37f, 1.0f}) {
        device.draws.clear();
        expected.draw(device, Mat4{1}, {}, 0.75f, 1.0f, &camera, TreeModel::Pass::All, blend);
        const auto draws = device.draws;
        device.draws.clear();
        actual.draw(device, Mat4{1}, {}, 0.75f, 1.0f, &camera, TreeModel::Pass::All, blend);
        REQUIRE(device.draws.size() == draws.size());
        for (usize i = 0; i < draws.size(); ++i) {
            CHECK(device.draws[i].texture == draws[i].texture);
            CHECK(device.draws[i].state == draws[i].state);
            CHECK(device.draws[i].transform == draws[i].transform);
            CHECK(device.draws[i].vertices == draws[i].vertices);
        }
    }
    device.draws.clear();
}

/** Test-owned identities for the immutable bindings of one already-loaded archive.
 * Lookups compare addresses only to identify owned objects; the digest receives
 * authored names/indices, never allocation addresses or device-specific handles. */
struct FigureResourceNames {
    ItemArchive& archive;
    std::string_view archiveIdentity;
    const Texture* white;
    std::vector<std::pair<const Texture*, u32>> textures;
    std::array<usize, 4> visits{};
    bool allowTextures = true;

    FigureResourceNames(ItemArchive& source, test::FakeRenderDevice& device,
                        std::string_view identity)
        : archive(source), archiveIdentity(identity), white(&device.whiteTexture()) {
        // Preload before recording any hashes. The resolver itself must not upload.
        for (u32 i = 0; i < source.textures.size(); ++i) {
            if (!source.textures.entry(i).external()) {
                textures.emplace_back(&source.textures.texture(device, i), i);
            }
        }
    }

    static bool append(NativeStateDigest& digest, std::type_index type, const void* resource,
                       void* context) {
        auto& names = *static_cast<FigureResourceNames*>(context);
        if (type == typeid(ItemArchive)) {
            if (resource != &names.archive) {
                return false;
            }
            ++names.visits[0];
            digest.values(std::string_view{"archive"}, names.archiveIdentity);
            return true;
        }
        if (type == typeid(TreeInfo)) {
            for (u32 i = 0; i < names.archive.trees.size(); ++i) {
                const auto& tree = names.archive.trees.tree(i);
                if (resource == &tree) {
                    ++names.visits[1];
                    digest.values(std::string_view{"tree"}, names.archiveIdentity, i, tree.name);
                    return true;
                }
            }
            return false;
        }
        if (type == typeid(TreeSequenceInfo)) {
            for (u32 i = 0; i < names.archive.trees.size(); ++i) {
                const auto& tree = names.archive.trees.tree(i);
                for (u32 j = 0; j < tree.sequences.size(); ++j) {
                    if (resource == &tree.sequences[j]) {
                        ++names.visits[2];
                        digest.values(std::string_view{"sequence"}, names.archiveIdentity, i,
                                      tree.name, j, tree.sequences[j].name);
                        return true;
                    }
                }
            }
            return false;
        }
        if (type == typeid(Texture) && names.allowTextures) {
            if (resource == names.white) {
                ++names.visits[3];
                digest.value(std::string_view{"builtin-white"});
                return true;
            }
            for (const auto& [texture, index] : names.textures) {
                if (resource == texture) {
                    ++names.visits[3];
                    digest.values(std::string_view{"texture"}, names.archiveIdentity, index,
                                  names.archive.textures.entry(index).name);
                    return true;
                }
            }
        }
        return false;
    }

    NativeStateDigest::Resources resources() { return {this, append}; }
};

template <class T>
NativeStateDigest::Digest figureDigest(const T& value, FigureResourceNames& names) {
    NativeStateDigest digest(names.resources());
    digest.value(value);
    const auto result = digest.checkedFinish();
    REQUIRE(result.has_value());
    return *result;
}

TEST_CASE("native fixture values rewind animation attachments particles and changing bindings",
          "[local-rewind][item-figure-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(figureArchive()));
    test::FakeRenderDevice device;
    ItemFigure native;
    ItemFigure replay;
    std::array<ItemFigure, 16> history;
    const auto tick = [&](ItemFigure& figure, u32 frame, bool wrong) {
        figure.capturePresentation();
        ItemInstance instance;
        instance.position = Vec3{static_cast<f32>(frame), 2, -4};
        if (frame % 32 == 0 || wrong) {
            REQUIRE(figure.place(device, archive, wrong ? "RELIC" : "CHEST", instance, nullptr));
            figure.gateParticlesOnSequence(!wrong);
        }
        if (!wrong && frame % 32 == 2) {
            figure.play(1, false);
        }
        if (!wrong && frame % 32 == 14) {
            figure.play(2, false); // Empty OPEN keeps ACTIVE's final socket and texture frame.
        }
        if (!wrong && frame % 32 == 24) {
            REQUIRE(figure.placeStaticFallback(device, archive, "BLADE", instance, nullptr, 0));
        }
        figure.placeAt(itemPlacement(Vec3{static_cast<f32>(frame), wrong ? 20.0f : -3.0f, 7},
                                     Vec3{0.2f, static_cast<f32>(frame) * 0.01f, 0}));
        figure.update(1.0f / 60);
    };
    u32 frame = 0;
    for (u32 window = 0; window < 96; ++window) {
        CAPTURE(window, frame);
        auto& saved = history[window % history.size()];
        saved = replay;
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            tick(replay, frame + i, true);
        }
        replay = saved;
        compare(replay, native, archive, device);
        for (u32 i = 0; i < distance; ++i) {
            tick(native, frame + i, false);
            tick(replay, frame + i, false);
            compare(replay, native, archive, device);
        }
        frame += distance;
    }
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("copied fixture drawings and texture clocks cannot change the historical figure",
          "[local-rewind][item-figure-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(figureArchive()));
    test::FakeRenderDevice device;
    ItemFigure original;
    REQUIRE(original.place(device, archive, "CHEST", {}, nullptr));
    original.play(1, false);
    original.update(0.25f);
    original.play(2, false);
    const ItemFigure saved = original;
    const ItemFigure reference = original;
    REQUIRE(saved.equalLocal(reference));
    original.play(0, true);
    original.update(0.3f);
    CHECK_FALSE(original.equalLocal(saved));
    original.draw(device, Mat4{1}, {});
    CHECK(saved.equalLocal(reference));
    compare(saved, reference, archive, device);
    original = saved;
    CHECK(original.equalLocal(saved));
    compare(original, reference, archive, device);
    CHECK(original.equalLocal(saved)); // Drawing did not alter native values.
    original.update(0.75f);
    CHECK(original.textureAnimator().clock() != saved.textureAnimator().clock());
    compare(saved, reference, archive, device);
    const auto uploads = device.texturesCreated;
    for (s32 i = 0; i < 64; ++i) {
        original = saved;
        REQUIRE(original.place(device, archive, "CHEST", {}, nullptr));
    }
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("fixture equality rejects lookalike but independently owned artwork",
          "[local-rewind][item-figure-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(figureArchive()));
    test::FakeRenderDevice device;
    ItemFigure original;
    ItemFigure independent;
    REQUIRE(original.place(device, archive, "CHEST", {}, nullptr));
    REQUIRE(independent.place(device, archive, "CHEST", {}, nullptr));
    CHECK_FALSE(original.equalLocal(independent));
    const ItemFigure saved = original;
    CHECK(original.equalLocal(saved));
    original.gateParticlesOnSequence(true);
    CHECK_FALSE(original.equalLocal(saved));
    original = saved;
    original.capturePresentation();
    CHECK_FALSE(original.equalLocal(saved));
    original = saved;
    original.placeAt(glm::translate(Mat4{1}, Vec3{1, 2, 3}));
    CHECK_FALSE(original.equalLocal(saved));
}

TEST_CASE("native figure digest uses semantic bindings across independent archives and devices",
          "[native-state-digest][item-figure-native-digest]") {
    const auto path = figureArchive();
    test::FakeRenderDevice firstDevice;
    test::FakeRenderDevice secondDevice;
    ItemArchive firstArchive;
    ItemArchive secondArchive;
    REQUIRE(firstArchive.load(path));
    REQUIRE(secondArchive.load(path));
    FigureResourceNames firstNames(firstArchive, firstDevice, "figure-fixture");
    FigureResourceNames secondNames(secondArchive, secondDevice, "figure-fixture");
    ItemFigure first;
    ItemFigure second;
    REQUIRE(first.place(firstDevice, firstArchive, "CHEST", {}, nullptr));
    REQUIRE(second.place(secondDevice, secondArchive, "CHEST", {}, nullptr));
    first.play(1, false);
    second.play(1, false);
    first.capturePresentation();
    second.capturePresentation();
    first.update(1.0f / 30);
    second.update(1.0f / 30);
    REQUIRE(first.particles().field().particleCount() > 0);
    REQUIRE(first.presentation().archive != second.presentation().archive);
    REQUIRE(first.presentation().tree != second.presentation().tree);
    REQUIRE(first.sequenceInfo(1) != second.sequenceInfo(1));
    REQUIRE(first.particles().field().textureOf(0) != second.particles().field().textureOf(0));
    CHECK_FALSE(first.equalLocal(second)); // Local equality deliberately requires one binding.
    const auto initial = figureDigest(first, firstNames);
    REQUIRE(figureDigest(second, secondNames) == initial);
    for (usize kind = 0; kind < firstNames.visits.size(); ++kind) {
        CAPTURE(kind);
        CHECK(firstNames.visits[kind] > 0);
        CHECK(secondNames.visits[kind] > 0);
    }

    const auto firstUploads = firstDevice.texturesCreated;
    const auto secondUploads = secondDevice.texturesCreated;
    const ItemFigure firstSaved = first;
    const ItemFigure secondSaved = second;
    const auto tick = [](ItemFigure& figure, s32 index) {
        figure.capturePresentation();
        figure.placeAt(itemPlacement({static_cast<f32>(index), 2, -4}, {0.1f, 0.2f, 0}));
        if (index == 4) {
            figure.play(2, false); // Empty OPEN retains the preceding live pose.
        }
        figure.update(1.0f / 60);
    };
    std::array<NativeStateDigest::Digest, 8> expected{};
    CameraFrame camera;
    camera.right = {1, 0, 0};
    camera.up = {0, 1, 0};
    for (s32 index = 0; index < 8; ++index) {
        tick(first, index);
        expected[index] = figureDigest(first, firstNames);
        // Hash without any draw, then different render-only interpolation samples.
        for (const f32 alpha : {0.0f, 0.37f, 1.0f}) {
            CAPTURE(index, alpha);
            firstDevice.draws.clear();
            first.draw(firstDevice, Mat4{1}, {}, 0.75f, 1, &camera, TreeModel::Pass::All, alpha);
            CHECK(figureDigest(first, firstNames) == expected[index]);
        }
    }
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        second = secondSaved;
        second.play(0, true);
        second.gateParticlesOnSequence(true);
        second.placeAt(glm::translate(Mat4{1}, Vec3{-8, 20, 4}));
        for (s32 index = 0; index < distance; ++index) {
            second.update(0.05f);
        }
        REQUIRE(figureDigest(second, secondNames) != expected[distance - 1]);
        second = secondSaved; // Native copy, not the diagnostic prepare/restore path.
        REQUIRE(second.equalLocal(secondSaved));
        REQUIRE(figureDigest(second, secondNames) == initial);
        for (s32 index = 0; index < distance; ++index) {
            tick(second, index);
            CHECK(figureDigest(second, secondNames) == expected[index]);
        }
    }
    first = firstSaved;
    CHECK(figureDigest(first, firstNames) == initial);
    CHECK(firstDevice.texturesCreated == firstUploads);
    CHECK(secondDevice.texturesCreated == secondUploads);
    CHECK(firstDevice.textureUpdates == 0);
    CHECK(secondDevice.textureUpdates == 0);

    // The resolver supplies stage-scoped semantic identity, not merely a kind tag.
    second = secondSaved;
    secondNames.archiveIdentity = "other-archive";
    CHECK(figureDigest(second, secondNames) != initial);
    secondNames.archiveIdentity = "figure-fixture";
    CHECK(figureDigest(second, secondNames) == initial);

    NativeStateDigest missing;
    missing.value(first);
    CHECK_FALSE(missing.checkedFinish().has_value());
    NativeStateDigest wrongOwner(firstNames.resources());
    wrongOwner.value(second);
    CHECK_FALSE(wrongOwner.checkedFinish().has_value());
    firstNames.allowTextures = false;
    NativeStateDigest unknownTexture(firstNames.resources());
    unknownTexture.value(first);
    CHECK_FALSE(unknownTexture.checkedFinish().has_value());
    firstNames.allowTextures = true;
    CHECK(figureDigest(first, firstNames) == initial);
}

TEST_CASE("native figure digest detects live clocks poses particles and emitter decisions",
          "[native-state-digest][item-figure-native-digest]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(figureArchive()));
    FigureResourceNames names(archive, device, "figure-fixture");
    ItemFigure figure;
    REQUIRE(figure.place(device, archive, "CHEST", {}, nullptr));
    const auto saved = figure;
    const auto original = figureDigest(figure, names);
    figure.capturePresentation();
    CHECK(figureDigest(figure, names) != original);
    figure = saved;
    figure.gateParticlesOnSequence(true);
    CHECK(figureDigest(figure, names) != original);
    figure = saved;
    figure.tilt(0.125f, 0.25f);
    CHECK(figureDigest(figure, names) != original);
    figure = saved;
    figure.update(1.0f / 120);
    CHECK(figureDigest(figure, names) != original);
    figure = saved;
    figure.play(1, false);
    figure.update(1.0f / 30);
    REQUIRE(figure.particles().field().particleCount() > 0);
    CHECK(figureDigest(figure, names) != original);
    const auto moving = figure;
    figure.update(1.0f / 30);
    REQUIRE(figure.nodeTransform("NULL1") != moving.nodeTransform("NULL1"));
    CHECK(figureDigest(figure, names) != figureDigest(moving, names));
    figure = moving;
    CHECK(figureDigest(figure, names) == figureDigest(moving, names));

    const auto& tree = *figure.presentation().tree;
    TreePose pose;
    pose.evaluate(tree, 1, 0);
    const auto restPose = pose;
    pose.evaluate(tree, 1, 1);
    CHECK(figureDigest(pose, names) != figureDigest(restPose, names));
    pose = restPose;
    CHECK(figureDigest(pose, names) == figureDigest(restPose, names));

    AnimationPlayer animation;
    animation.start(tree.sequences[1], 1);
    const auto clock = animation;
    animation.advance(1.0f / 120, false);
    CHECK(figureDigest(animation, names) != figureDigest(clock, names));
    animation = clock;
    CHECK(figureDigest(animation, names) == figureDigest(clock, names));

    const auto nativeParticles = figure.particles();
    auto particles = nativeParticles;
    particles.step(1.0f / 120, figure.transform(), restPose.matrices());
    CHECK(figureDigest(particles, names) != figureDigest(nativeParticles, names));
    particles = nativeParticles;
    particles.stop();
    CHECK(figureDigest(particles, names) != figureDigest(nativeParticles, names));
    particles = nativeParticles;
    CHECK(figureDigest(particles, names) == figureDigest(nativeParticles, names));

    const auto& field = nativeParticles.field();
    auto changedField = field;
    changedField.setNode(0, glm::translate(Mat4{1}, Vec3{1, 2, 3}));
    CHECK(figureDigest(changedField, names) != figureDigest(field, names));
    changedField = field;
    changedField.setSpriteScale(0, 0.5f);
    CHECK(figureDigest(changedField, names) != figureDigest(field, names));
    changedField = field;
    const auto alternateTexture = archive.textures.find("LM");
    REQUIRE(alternateTexture.has_value());
    changedField.setTexture(0, archive.textures.texture(device, *alternateTexture));
    REQUIRE(changedField.textureOf(0) != field.textureOf(0));
    CHECK(figureDigest(changedField, names) != figureDigest(field, names));
    changedField = field;
    CHECK(figureDigest(changedField, names) == figureDigest(field, names));

    const auto descriptor = field.emitter(0).descriptor();
    ParticleEmitter firstSeed;
    ParticleEmitter secondSeed;
    firstSeed.start(descriptor, Mat4{1}, 1);
    secondSeed.start(descriptor, Mat4{1}, 2);
    REQUIRE(firstSeed.particles().empty());
    REQUIRE(secondSeed.particles().empty());
    CHECK(figureDigest(firstSeed, names) != figureDigest(secondSeed, names));
    const auto emitterSaved = firstSeed;
    firstSeed.step(1);
    CHECK(figureDigest(firstSeed, names) != figureDigest(emitterSaved, names));
    firstSeed = emitterSaved;
    CHECK(figureDigest(firstSeed, names) == figureDigest(emitterSaved, names));
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("sequence-less item draws cannot change native particle texture bindings",
          "[native-state-digest][item-figure-native-digest][local-rewind]") {
    const auto path = test::sampleLevel("item-figure-sequence-free-draw");
    writeTextFile(path / "animations.json", R"({
      "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":4}],
      "particles":[
        {"enables":16928,"texture":"STONE","particleLife":[1,0.1],
         "rate":[30,30,30,30],"flags":0,"flagMask":2176},
        {"enables":16928,"texture":"STONE","particleLife":[1,0.1],
         "rate":[30,30,30,30],"flags":128,"flagMask":2176}],
      "trees":[
        {"name":"CUTOUT","nodes":[
          {"name":"ROOT","parent":-1,"position":[0,0,0],"particle":0}]},
        {"name":"SOFT","nodes":[
          {"name":"ROOT","parent":-1,"position":[0,0,0],"particle":1}]}]})");
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(path));
    FigureResourceNames names(archive, device, "sequence-free-fixture");
    CameraFrame camera;
    camera.right = {1, 0, 0};
    camera.up = {0, 1, 0};
    for (const auto* const name : {"CUTOUT", "SOFT"}) {
        CAPTURE(name);
        ItemFigure figure;
        REQUIRE(figure.place(device, archive, name, {}, nullptr));
        REQUIRE(figure.sequenceCount() == 0);
        REQUIRE(figure.textureAnimator().size() == 1);
        CHECK(figure.particles().field().textureOf(0) == figure.textureAnimator().motion(0).frame);
        figure.capturePresentation();
        figure.update(0.1f);
        REQUIRE(figure.particles().field().particleCount() > 0);
        const auto* nativeTexture = figure.particles().field().textureOf(0);
        const auto* animatedTexture = figure.textureAnimator().motion(0).frame;
        REQUIRE(animatedTexture != nullptr);
        REQUIRE(animatedTexture == nativeTexture);
        REQUIRE(figure.particles().field().emitter(0).descriptor().depthWrite);
        const auto original = figureDigest(figure, names);
        const ItemFigure saved = figure;
        ItemFigure undrawn = figure;
        const auto uploads = device.texturesCreated;

        // Free-running textures must be populated by place/update, including
        // sequence-less trees, rather than first appearing in a native-step draw.
        device.draws.clear();
        figure.draw(device, Mat4{1}, {}, 1, 1, &camera, TreeModel::Pass::All, -1);
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK(draw.texture == animatedTexture);
            CHECK(draw.state.nextTexture == nullptr);
            CHECK(draw.state.textureBlend == 0);
        }
        CHECK(figure.particles().field().textureOf(0) == nativeTexture);
        CHECK(figureDigest(figure, names) == original);
        CHECK(figure.equalLocal(saved));

        // Mixed rendering cadence cannot affect the next native tick, including
        // native-step draws immediately after interpolated overrides.
        for (s32 tick = 0; tick < 8; ++tick) {
            CAPTURE(tick);
            figure.capturePresentation();
            undrawn.capturePresentation();
            figure.update(1.0f / 60);
            undrawn.update(1.0f / 60);
            const auto beforeDrawing = figureDigest(undrawn, names);
            REQUIRE(figureDigest(figure, names) == beforeDrawing);
            CHECK(figure.particles().field().textureOf(0) ==
                  figure.textureAnimator().motion(0).frame);
            for (const f32 alpha : {0.0f, 0.37f, 1.0f, -1.0f}) {
                CAPTURE(alpha);
                device.draws.clear();
                figure.draw(device, Mat4{1}, {}, 1, 1, &camera, TreeModel::Pass::All, alpha);
                CHECK(figureDigest(figure, names) == beforeDrawing);
                CHECK(figure.equalLocal(undrawn));
                if (alpha < 0 || std::string_view{name} == "CUTOUT") {
                    REQUIRE_FALSE(device.draws.empty());
                    const auto* current = figure.textureAnimator().motion(0).frame;
                    for (const auto& draw : device.draws) {
                        CHECK(draw.texture == current);
                        CHECK(draw.state.nextTexture == nullptr);
                        CHECK(draw.state.textureBlend == 0);
                    }
                }
            }
        }
        figure = saved;
        CHECK(figureDigest(figure, names) == original);
        CHECK(device.texturesCreated == uploads);
        CHECK(device.textureUpdates == 0);
    }
}
} // namespace
