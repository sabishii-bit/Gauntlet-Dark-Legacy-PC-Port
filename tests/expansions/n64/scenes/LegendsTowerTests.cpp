#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/formats/LegendsN64Tower.h"
#include "expansions/shared/scenes/LegendsPreview.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsMap towerMap(const std::filesystem::path& directory) {
    const auto models = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                             readFile(directory / "ipr.rom"));
    for (auto& texture : formats::decodeLegendsN64Textures(models)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

void finiteCamera(const WorldCamera& camera) {
    REQUIRE(std::isfinite(camera.position.x));
    REQUIRE(std::isfinite(camera.position.y));
    REQUIRE(std::isfinite(camera.position.z));
    REQUIRE(std::isfinite(camera.pitch));
    REQUIRE(std::isfinite(camera.yaw));
    REQUIRE(std::isfinite(camera.roll));
}

void sameCamera(const WorldCamera& actual, const WorldCamera& expected) {
    CHECK(glm::distance(actual.position, expected.position) < .001f);
    CHECK(std::abs(std::remainder(actual.yaw - expected.yaw, 2.0f * kPi)) < .0001f);
    CHECK(std::abs(actual.pitch - expected.pitch) < .0001f);
    CHECK(std::abs(actual.roll - expected.roll) < .0001f);
}

void renderTower(const LegendsPreview& preview, test::FakeRenderDevice& device,
                 const std::filesystem::path& characterRoot) {
    device.draws.clear();
    preview.render(device, Mat4{1}, 640, 448, kPi / 2.0f);
    REQUIRE_FALSE(device.draws.empty());
    bool texturedWorld = false;
    std::vector<ImmediateVertex> overlay;
    const auto hudTransform = makeVirtualScreenTransform(Mat4{1}, 512, 384, 640, 448);
    for (const auto& draw : device.draws) {
        REQUIRE(draw.texture != nullptr);
        if (draw.state.depthTest && draw.texture != &device.whiteTexture() &&
            !draw.vertices.empty()) {
            texturedWorld = true;
        }
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                REQUIRE(std::isfinite(draw.transform[column][row]));
            }
        }
        for (const auto& vertex : draw.vertices) {
            REQUIRE(std::isfinite(vertex.position.x));
            REQUIRE(std::isfinite(vertex.position.y));
            REQUIRE(std::isfinite(vertex.position.z));
            REQUIRE(std::isfinite(vertex.uv.x));
            REQUIRE(std::isfinite(vertex.uv.y));
        }
        // Canvas sprites deliberately use depth 0.5 with depth testing enabled;
        // the virtual-screen transform distinguishes them from perspective art.
        if (draw.transform == hudTransform) {
            overlay.insert(overlay.end(), draw.vertices.begin(), draw.vertices.end());
        }
    }
    CHECK(texturedWorld);

    // Keep the ordinary character card, but no importer-specific pickup/debug HUD.
    // Derive its expected geometry from the real painter, not a hard-coded draw count.
    test::FakeRenderDevice hudDevice;
    StatusBoxPainter status;
    REQUIRE(status.load(hudDevice, characterRoot, nullptr));
    status.preloadStatus();
    const auto& save = preview.traversal()->actor().save();
    const auto& inventory = save.progress().inventory;
    StatusBoxView view;
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = save.character;
    view.color = save.color;
    view.name = save.name;
    view.level = experienceLevel(save.experience());
    view.gold = save.gold;
    view.health = save.health();
    view.keys = inventory.keys;
    view.potions = static_cast<s32>(inventory.potions.size());
    view.potionKind = inventory.nextPotion();
    Canvas canvas;
    canvas.begin(hudDevice, hudTransform);
    status.draw(canvas, 0, view, true);
    canvas.end();
    std::vector<ImmediateVertex> expected;
    for (const auto& draw : hudDevice.draws) {
        expected.insert(expected.end(), draw.vertices.begin(), draw.vertices.end());
    }
    REQUIRE_FALSE(expected.empty());
    CHECK(overlay == expected);
}

TEST_CASE("Native N64 Tower visit uses its radial camera at 60 and 144 Hz",
          "[legends][tower][camera][preview][assets]") {
    const auto characterRoot = test::assetOrSkip("PLAYERS").parent_path();
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Tower extraction is not installed");
    }
    auto map = towerMap(directory);
    REQUIRE_FALSE(map.collision.empty());
    REQUIRE_FALSE(map.starts.empty());
    REQUIRE_FALSE(map.textures.empty());
    test::FakeRenderDevice sixtyDevice;
    auto adapter = LegendsSceneAdapter::load(sixtyDevice, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter != nullptr);
    // Sumner's script-created cylinder is separate from worlds.wad. Walk
    // through the body and across it at speed without changing the player's Y.
    CHECK(adapter->resolveMovement({10, 0, -8}, {6, 0, -8}, .75f, 5) == Vec3(8.25f, 0, -8));
    CHECK(adapter->resolveMovement({10, 0, -8}, {2, 0, -8}, .75f, 5) == Vec3(8.25f, 0, -8));
    CHECK(adapter->resolveMovement({10, 7, -8}, {2, 7, -8}, .75f, 5) == Vec3(2, 7, -8));
    REQUIRE(adapter->cameraConfig());
    const auto native = *adapter->cameraConfig();
    REQUIRE(native.tower);
    REQUIRE(native.markers.size() == 34);
    REQUIRE(native.towerExits.size() == 5);
    CHECK(native.towerExits.front() == Vec3{90, 2, -52});
    CHECK(native.towerExits.back() == Vec3{0, -5, 104});
    REQUIRE(adapter->fixtureCount() > 0);
    CHECK(adapter->pickups().empty());

    PartyMember member;
    member.save.name = "TEST";
    member.save.progress().health = 500;
    const auto before = member.save;
    LegendsPreview sixty;
    sixty.openPlayable(sixtyDevice, map, characterRoot, member, std::move(adapter));
    REQUIRE(sixty.isOpen());
    REQUIRE(sixty.playable());
    REQUIRE(sixty.traversal());
    REQUIRE(sixty.traversal()->authoredStart());
    const Vec3 entrance = sixty.traversal()->actor().position();
    CHECK(entrance.x == Approx(-4.5f));
    CHECK(entrance.z == Approx(-15.5f));
    LegendsCamera reference;
    reference.reset(native, entrance);
    sameCamera(sixty.camera(), reference.camera());
    CHECK(reference.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(sixty.camera().pitch == Approx(.05f));
    CHECK(reference.distance() == 55);
    WorldCamera fallback;
    fallback.pitch = kPi * .25f;
    fallback.position = sixty.traversal()->actor().followPoint() - fallback.forward() * 32.0f;
    CHECK(glm::distance(sixty.camera().position, fallback.position) > 10);

    Input input;
    input.beginPoll();
    input.setKey(Key::D, true);
    GameConfig config;
    config.controls[0].device = "keyboard";
    config.controls[0].scheme = KeyboardScheme::Robotron;
    const auto held = sixty.camera();
    for (s32 frame = 0; frame < 8; ++frame) {
        sixty.updatePlayable(input, .25f, true, config);
        CHECK(sixty.traversal()->actor().position() == entrance);
        CHECK(sixty.camera() == held);
    }
    renderTower(sixty, sixtyDevice, characterRoot);

    struct Sample {
        Vec3 feet;
        WorldCamera camera;
    };
    std::vector<Sample> commonTimes;
    // Independent 30Hz clock: each native camera tick sees the actor after two
    // completed 60Hz physics ticks. Odd ticks are interpolation only.
    for (s32 frame = 1; frame <= 120; ++frame) {
        sixty.updatePlayable(input, 1.0f / 60.0f, false, config);
        const auto feet = sixty.traversal()->actor().position();
        if (frame % 2 == 0) {
            reference.update(feet, 1.0f / 30.0f);
        }
        const auto expected = reference.sample(frame % 2 == 0 ? 0.0f : 1.0f / 60.0f);
        finiteCamera(sixty.camera());
        sameCamera(sixty.camera(), expected);
        if (frame % 5 == 0) {
            commonTimes.push_back({feet, sixty.camera()});
        }
    }
    CHECK(glm::distance(sixty.traversal()->actor().position(), entrance) > 1);
    CHECK(glm::distance(sixty.camera().position, held.position) > .1f);
    CHECK(member.save == before);
    CHECK(sixty.traversal()->actor().save() == before);
    const auto movingCamera = sixty.camera();
    const auto movingFeet = sixty.traversal()->actor().position();
    for (s32 frame = 0; frame < 8; ++frame) {
        sixty.updatePlayable(input, .25f, true, config);
        CHECK(sixty.traversal()->actor().position() == movingFeet);
        sameCamera(sixty.camera(), movingCamera);
    }
    renderTower(sixty, sixtyDevice, characterRoot);

    auto fastMap = towerMap(directory);
    test::FakeRenderDevice fastDevice;
    auto fastAdapter =
        LegendsSceneAdapter::load(fastDevice, LegendsPlatform::N64, directory, fastMap);
    REQUIRE(fastAdapter->cameraConfig());
    LegendsPreview fast;
    fast.openPlayable(fastDevice, fastMap, characterRoot, member, std::move(fastAdapter));
    usize sample = 0;
    usize movedRenderFrames = 0;
    for (s32 frame = 1; frame <= 288; ++frame) {
        const auto previous = fast.camera();
        fast.updatePlayable(input, 1.0f / 144.0f, false, config);
        finiteCamera(fast.camera());
        if (frame == 25) {
            // This falls between physics ticks, unlike the entrance/60Hz holds.
            // Opening the console must not discard the render interpolation
            // fraction or consume it when normal updates resume.
            const auto fractionalCamera = fast.camera();
            const auto fractionalFeet = fast.traversal()->actor().position();
            for (s32 heldFrame = 0; heldFrame < 3; ++heldFrame) {
                fast.updatePlayable(input, .25f, true, config);
                CHECK(fast.camera() == fractionalCamera);
                CHECK(fast.traversal()->actor().position() == fractionalFeet);
            }
        }
        if (glm::distance(previous.position, fast.camera().position) > .00001f) {
            ++movedRenderFrames;
        }
        if (frame % 12 == 0) {
            REQUIRE(sample < commonTimes.size());
            CHECK(glm::distance(fast.traversal()->actor().position(), commonTimes[sample].feet) <
                  .001f);
            sameCamera(fast.camera(), commonTimes[sample].camera);
            ++sample;
        }
    }
    CHECK(sample == commonTimes.size());
    CHECK(movedRenderFrames > 60); // Not a held 30Hz camera at a 144Hz presentation rate.
    renderTower(fast, fastDevice, characterRoot);
    CHECK(member.save == before);
    sixty.close();
    fast.close();
    CHECK_FALSE(sixty.isOpen());
    CHECK_FALSE(fast.playable());
    fastDevice.draws.clear();
    fast.render(fastDevice, Mat4{1}, 640, 448, kPi / 2.0f);
    CHECK(fastDevice.draws.empty());
}

TEST_CASE("Native N64 Tower residents animate geometry while locked doors retain their held pose",
          "[legends][tower][animation][preview][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Tower extraction is not installed");
    }
    const auto tower = formats::decodeLegendsN64Tower(readFile(root / "_program/Tower.bin"));
    const auto sumnerTextures =
        formats::decodeLegendsN64Textures(readFile(root / tower.sumner.models / "objects.wad"));
    const auto towerTextures =
        formats::decodeLegendsN64Textures(readFile(root / tower.podium.models / "objects.wad"));
    auto map = towerMap(directory);
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter != nullptr);
    for (const auto& warning : adapter->warnings()) {
        INFO(warning);
        REQUIRE(warning.find("Unbound fixture: WIZMOVES:") == std::string::npos);
        REQUIRE(warning.find("Unbound fixture: PODIUM:") == std::string::npos);
        REQUIRE(warning.find("Tower scripted idle artwork is incomplete") == std::string::npos);
    }
    REQUIRE(adapter->cameraConfig());
    LegendsCamera camera;
    camera.reset(*adapter->cameraConfig(), {-4.5f, 0, -15.5f});
    const auto frame = CameraFrame::of(camera.camera());

    // Identify real native materials from their decoded image bytes. This does
    // not depend on private fixture indices, draw ordering, or pointer identity
    // between independently loaded texture banks.
    enum class Material { Other, Sumner, Book, Tower };
    std::map<const Texture*, Material> materials;
    const auto material = [&](const Texture* texture) {
        const auto existing = materials.find(texture);
        if (existing != materials.end()) {
            return existing->second;
        }
        const auto* pixels = dynamic_cast<const test::FakeTexture*>(texture);
        REQUIRE(pixels != nullptr);
        const auto matches = [&](const formats::LegendsN64Texture& candidate) {
            return !candidate.external && !candidate.image.pixels.empty() &&
                   candidate.image.width == pixels->width() &&
                   candidate.image.height == pixels->height() &&
                   candidate.image.pixels == pixels->pixels;
        };
        Material result = Material::Other;
        if (std::ranges::any_of(sumnerTextures, [&](const auto& candidate) {
                return candidate.name.starts_with("SUM_") && matches(candidate);
            })) {
            result = Material::Sumner;
        } else {
            for (const auto& candidate : towerTextures) {
                if (matches(candidate)) {
                    result = candidate.name == "KEYMUSICSHEET" ? Material::Book : Material::Tower;
                    break;
                }
            }
        }
        materials.emplace(texture, result);
        return result;
    };
    struct Geometry {
        std::vector<Vec3> sumner;
        std::vector<Vec3> book;
        std::array<std::vector<Vec3>, 4> doors;
    };
    const auto capture = [&] {
        device.draws.clear();
        adapter->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
        adapter->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
        Geometry result;
        for (const auto& draw : device.draws) {
            if (draw.vertices.empty()) {
                continue;
            }
            Vec3 centre{0};
            for (const auto& vertex : draw.vertices) {
                REQUIRE(std::isfinite(vertex.position.x));
                REQUIRE(std::isfinite(vertex.position.y));
                REQUIRE(std::isfinite(vertex.position.z));
                centre += vertex.position;
            }
            centre /= static_cast<f32>(draw.vertices.size());
            std::vector<Vec3>* positions = nullptr;
            switch (material(draw.texture)) {
            case Material::Sumner: positions = &result.sumner; break;
            case Material::Book: positions = &result.book; break;
            case Material::Tower:
                for (usize i = 0; i < tower.doors.size(); ++i) {
                    const auto delta = centre - tower.doors[i].triggerPosition;
                    if (glm::length(Vec2{delta.x, delta.z}) < 15) {
                        positions = &result.doors[i];
                        break;
                    }
                }
                break;
            case Material::Other: break;
            }
            if (positions) {
                for (const auto& vertex : draw.vertices) {
                    positions->push_back(vertex.position);
                }
            }
        }
        return result;
    };
    const auto reasonableResident = [&](std::span<const Vec3> positions) {
        REQUIRE_FALSE(positions.empty());
        Vec3 low = positions.front();
        Vec3 high = low;
        for (const auto& position : positions) {
            const Vec3 relative = position - tower.sumner.position;
            REQUIRE(glm::length(relative) < 20);
            low = glm::min(low, position);
            high = glm::max(high, position);
        }
        // Guards missing/degenerate geometry and 32x/64x coordinate errors,
        // without imposing a silhouette from the GameCube character artwork.
        CHECK(glm::distance(low, high) > 1);
    };
    const auto initial = capture();
    reasonableResident(initial.sumner);
    reasonableResident(initial.book);
    const auto height = [](std::span<const Vec3> positions) {
        const auto bounds = std::ranges::minmax_element(positions, {}, &Vec3::y);
        return bounds.max->y - bounds.min->y;
    };
    // Native mesh and BODY coordinates use 1/64. Tower.bin e0860198 never
    // scales this tree: a GDL master character's 1.2 scale must not inflate it.
    const f32 nativeSumnerHeight = height(initial.sumner);
    CHECK(nativeSumnerHeight > 5.0f);
    CHECK(nativeSumnerHeight < 5.6f);
    for (usize door = 0; door < tower.doors.size(); ++door) {
        INFO(tower.doors[door].openingSequence);
        REQUIRE_FALSE(initial.doors[door].empty());
    }
    const auto moved = [](std::span<const Vec3> before, std::span<const Vec3> after) {
        if (before.size() != after.size()) {
            return false; // A variant/count change alone is not proof of animated vertices.
        }
        for (usize i = 0; i < before.size(); ++i) {
            if (glm::distance(before[i], after[i]) > .001f) {
                return true;
            }
        }
        return false;
    };
    bool sumnerMoves = false;
    bool bookMoves = false;
    // The adapter's seeded native idle decisions must eventually select PAGE /
    // podium FLIP; allow complete READ/conjure cycles rather than force a private state.
    for (s32 tick = 1; tick <= 6000; ++tick) {
        adapter->capturePresentation();
        adapter->update(1.0f / 60.0f);
        if (tick % 15 != 0) {
            continue;
        }
        const auto current = capture();
        reasonableResident(current.sumner);
        reasonableResident(current.book);
        sumnerMoves |= moved(initial.sumner, current.sumner);
        bookMoves |= moved(initial.book, current.book);
        for (usize door = 0; door < tower.doors.size(); ++door) {
            INFO(tower.doors[door].openingSequence << " at tick " << tick);
            CHECK(current.doors[door] == initial.doors[door]);
        }
        if (sumnerMoves && bookMoves) {
            break;
        }
    }
    CHECK(sumnerMoves);
    CHECK(bookMoves);
}
} // namespace
