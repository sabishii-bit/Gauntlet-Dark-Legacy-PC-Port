#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/world/AnimationPlayer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/CritterWad.h"
#include "game/enemies/CombatantAssets.h"
#include "game/enemies/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr usize kType = 16;
constexpr usize kDescriptor = kType + 0x140;
constexpr usize kMove = kDescriptor + 0x30;
constexpr usize kPattern = kMove + 0x90;
constexpr usize kDirectory = kPattern + 0x50;

void putWord(std::vector<u8>& bytes, usize at, u32 value) {
    for (usize byte = 0; byte < 4; ++byte) {
        bytes.at(at + byte) = static_cast<u8>(value >> (byte * 8));
    }
}

std::vector<u8> tinyCritter() {
    test::ByteWriter writer;
    writer.putU32(static_cast<u32>(kDirectory)).putU32(4).putZeros(kDirectory - 8);
    for (const auto& [tag, at] :
         {std::pair{"TYPE", kType}, {"DESC", kDescriptor}, {"MOVE", kMove}, {"PTRN", kPattern}}) {
        writer.putU32(std::byteswap(fourcc(tag))).putU32(static_cast<u32>(at)).putU32(1).putU32(1);
    }
    auto bytes = writer.bytes();
    putWord(bytes, kDescriptor, 0x4D654F47); // GOeM: descriptor casing is independent of the path
    putWord(bytes, kDescriptor + 0x20, 3);
    putWord(bytes, kType + 0x5C, 0xC7F);
    putWord(bytes, kType + 0xA4, std::bit_cast<u32>(999.0f));
    putWord(bytes, kType + 0xAC, std::bit_cast<u32>(22.0f));
    putWord(bytes, kType + 0xE4, std::bit_cast<u32>(123.0f));
    putWord(bytes, kType + 0xF8, 2);
    putWord(bytes, kType + 0x110, 1);
    putWord(bytes, kType + 0x114, 1);
    putWord(bytes, kType + 0x11C, 0xFFFFFFFF);
    putWord(bytes, kMove, MoveDefinition::kReady);
    putWord(bytes, kMove + 0x30, 9); // a tab denotes no collision node
    putWord(bytes, kMove + 0x40, 7);
    putWord(bytes, kMove + 0x44, 11);
    putWord(bytes, kMove + 0x50, 0x000E0009);
    putWord(bytes, kPattern + 0x20, 0xFFFF0000); // move zero, then the terminator
    return bytes;
}

void emptyState(const CritterData& data) {
    REQUIRE_FALSE(data.loaded());
    REQUIRE(data.name().empty());
    REQUIRE(data.moves().empty());
    REQUIRE(data.patterns().empty());
    REQUIRE(data.parts().empty());
    REQUIRE(data.damages().empty());
    REQUIRE(data.sounds().empty());
    REQUIRE(data.typeFlags() == 0);
    REQUIRE(data.childIndex() == -1);
    REQUIRE(data.looks()[0].node.empty());
}

TEST_CASE("native critter tables win over stale exports and resolve legacy case-insensitive paths",
          "[native-assets][native-critter]") {
    const auto root = test::scratchDirectory("native-critter");
    std::filesystem::create_directories(root / "CRITTER");
    writeFile(root / "CRITTER/GOLEM.WAD", tinyCritter());
    writeTextFile(root / "CRITTER/GOLEM.json", "invalid stale export");
    CritterData data;
    REQUIRE(data.load(root / "critter/golem.json"));
    REQUIRE(data.name() == "GOLEM");
    REQUIRE(data.kind() == CombatantKind::Golem);
    REQUIRE(data.folder() == "goem");
    REQUIRE(data.maxHealth() == 123);
    REQUIRE(data.typeFlags() == 0xC7F);
    REQUIRE(data.shadowed());
    REQUIRE(data.movement().squareBounds);
    REQUIRE(data.movement().initialStepBasis);
    REQUIRE(data.movement().unrestrictedTurn);
    REQUIRE_FALSE(data.movement().home.has_value());
    REQUIRE(data.movement().roamRadius == 22);
    REQUIRE(data.meter().shown);
    REQUIRE(data.meter().backed);
    REQUIRE(data.meter().inWorld);
    REQUIRE(data.looks()[0].parent);
    REQUIRE(data.moves()[0].colnode.empty());
    REQUIRE(data.moves()[0].frameStart == 7);
    REQUIRE(data.moves()[0].frameEnd == 9);
    REQUIRE(data.moves()[0].frameStart2 == 11);
    REQUIRE(data.moves()[0].frameEnd2 == 14);
    REQUIRE(data.patterns()[0].moves == std::vector<s32>{0});
    REQUIRE(data.load(root / "CRITTER/GOLEM.WAD"));
    REQUIRE_FALSE(data.load(root / "critter/golem.json", 1));
    emptyState(data);
}

TEST_CASE("bad native critter data clears earlier and partial loads without export fallback",
          "[native-assets][native-critter]") {
    const auto root = test::scratchDirectory("native-critter-failure");
    const auto native = root / "GOLEM.WAD";
    const auto legacy = root / "GOLEM.json";
    writeTextFile(legacy, R"({"types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}]})");
    CritterData data;
    REQUIRE(data.load(legacy));
    auto bytes = tinyCritter();
    SECTION("truncated directory") {
        bytes.resize(7);
    }
    SECTION("invalid descriptor") {
        putWord(bytes, kType + 0x50, 4);
    }
    SECTION("negative move count") {
        putWord(bytes, kType + 0x110, 0xFFFF);
    }
    SECTION("empty move table") {
        putWord(bytes, kType + 0x110, 0);
    }
    SECTION("move range outside table") {
        putWord(bytes, kType + 0x110, 2);
    }
    SECTION("missing pattern range") {
        putWord(bytes, kType + 0x114, 2);
    }
    SECTION("invalid node range") {
        putWord(bytes, kType + 0x118, 1);
    }
    SECTION("invalid pattern after moves were populated") {
        putWord(bytes, kPattern + 0x20, 0xFFFF0001);
    }
    writeFile(native, bytes);
    REQUIRE_FALSE(data.load(legacy));
    emptyState(data);
}

TEST_CASE("legacy critter fixtures clear partially decoded failures", "[native-critter]") {
    const auto root = test::scratchDirectory("legacy-critter-failure");
    const auto file = root / "fixture.json";
    CritterData data;
    writeTextFile(file, R"({"name":"FIXTURE","types":[{"moveCount":1}],
        "descriptors":[{}],"moves":[{}],"sounds":[{"scale":"bad"}]})");
    REQUIRE_FALSE(data.load(file));
    emptyState(data);
}

auto targetValues(const TargetCriteria& t) {
    return std::tie(t.minDistance, t.maxDistance, t.yaw, t.minDot, t.maxVertical, t.minRateScale,
                    t.maxRateScale, t.maxHomeDistance);
}

auto moveValues(const MoveDefinition& m) {
    return std::tie(m.type, m.flags, m.priority, m.name, m.anim, m.colnode, m.frameStart,
                    m.frameEnd, m.frameStart2, m.frameEnd2, m.framePeriod, m.damage0, m.damage1,
                    m.link, m.interrupt, m.sound, m.soundFrame, m.sound2, m.sound2Frame, m.cooldown,
                    m.speed, m.turnRate, m.hold);
}

auto damageValues(const AttackDefinition& d) {
    return std::tie(d.type, d.behaviorFlags, d.flags, d.radius, d.maxDistance, d.minDistance, d.yaw,
                    d.minDot, d.pitch, d.offset, d.damage, d.speed, d.maxSpeed, d.gravity,
                    d.morphLife, d.yawSpread, d.sound, d.hitSound, d.morph, d.morphEnd);
}

auto effectValues(const CombatEffectDefinition& s) {
    return std::tie(s.tree, s.soundFormat, s.flags, s.link, s.offset, s.life, s.scale,
                    s.particleRate, s.particleSpeed, s.skinLoops);
}

auto partValues(const CritterPart& p) {
    return std::tie(p.node, p.position, p.radius, p.damageScale, p.healthScale, p.damageEffect,
                    p.flags, p.targetScoreScale, p.maxTargetDistance);
}

auto lookValues(const LookDefinition& l) {
    return std::tie(l.node, l.parent, l.yawRate, l.pitchRate, l.pitchBias);
}

auto meterValues(const HealthMeterDefinition& m) {
    return std::tie(m.pieces, m.advance, m.leftInset, m.rightInset, m.shown, m.backed, m.inWorld,
                    m.name, m.barOffset);
}

auto movementValues(const CritterMovement& m) {
    return std::tie(m.roamRadius, m.turnLimit, m.home, m.squareBounds, m.initialStepBasis,
                    m.unrestrictedTurn);
}

void equivalent(const CritterData& a, const CritterData& b) {
    REQUIRE(a.name() == b.name());
    REQUIRE(a.folder() == b.folder());
    REQUIRE(a.prefix() == b.prefix());
    REQUIRE(a.tree() == b.tree());
    REQUIRE(a.kind() == b.kind());
    REQUIRE(a.typeFlags() == b.typeFlags());
    REQUIRE(a.childIndex() == b.childIndex());
    REQUIRE(a.parentIndex() == b.parentIndex());
    REQUIRE(a.rootNode() == b.rootNode());
    REQUIRE(a.radius() == b.radius());
    REQUIRE(a.wallRadius() == b.wallRadius());
    REQUIRE(a.armor() == b.armor());
    REQUIRE(a.itemDamage() == b.itemDamage());
    REQUIRE(a.shieldFlags() == b.shieldFlags());
    REQUIRE(a.maxHealth() == b.maxHealth());
    REQUIRE(a.experience() == b.experience());
    REQUIRE(a.wakeThreshold() == b.wakeThreshold());
    REQUIRE(a.vertDrift() == b.vertDrift());
    REQUIRE(a.shadowed() == b.shadowed());
    REQUIRE(a.floorOffset() == b.floorOffset());
    REQUIRE(a.originOffset() == b.originOffset());
    REQUIRE(a.hitSoundFar() == b.hitSoundFar());
    REQUIRE(a.hitSoundClose() == b.hitSoundClose());
    REQUIRE(targetValues(a.sight()) == targetValues(b.sight()));
    REQUIRE(meterValues(a.meter()) == meterValues(b.meter()));
    REQUIRE(movementValues(a.movement()) == movementValues(b.movement()));
    REQUIRE(a.moves().size() == b.moves().size());
    for (usize i = 0; i < a.moves().size(); ++i) {
        CAPTURE(i);
        REQUIRE(moveValues(a.moves()[i]) == moveValues(b.moves()[i]));
        REQUIRE(targetValues(a.moves()[i].target) == targetValues(b.moves()[i].target));
    }
    REQUIRE(a.patterns().size() == b.patterns().size());
    for (usize i = 0; i < a.patterns().size(); ++i) {
        CAPTURE(i);
        REQUIRE(a.patterns()[i].moves == b.patterns()[i].moves);
        REQUIRE(a.patterns()[i].flags == b.patterns()[i].flags);
        REQUIRE(a.patterns()[i].cooldown == b.patterns()[i].cooldown);
        REQUIRE(targetValues(a.patterns()[i].target) == targetValues(b.patterns()[i].target));
    }
    REQUIRE(a.damages().size() == b.damages().size());
    for (usize i = 0; i < a.damages().size(); ++i) {
        CAPTURE(i);
        REQUIRE(damageValues(a.damages()[i]) == damageValues(b.damages()[i]));
    }
    REQUIRE(a.sounds().size() == b.sounds().size());
    for (usize i = 0; i < a.sounds().size(); ++i) {
        CAPTURE(i);
        REQUIRE(effectValues(a.sounds()[i]) == effectValues(b.sounds()[i]));
    }
    REQUIRE(a.parts().size() == b.parts().size());
    for (usize i = 0; i < a.parts().size(); ++i) {
        CAPTURE(i);
        REQUIRE(partValues(a.parts()[i]) == partValues(b.parts()[i]));
    }
    REQUIRE(a.looks().size() == b.looks().size());
    for (usize i = 0; i < a.looks().size(); ++i) {
        REQUIRE(lookValues(a.looks()[i]) == lookValues(b.looks()[i]));
    }
}

TEST_CASE("every native critter type preserves current exported runtime metadata",
          "[native-assets][native-critter][assets][unpacked]") {
    const auto directory = test::assetOrSkip("CRITTER");
    const char* reference = std::getenv("GDL_NATIVE_REFERENCE_DIR");
    if (reference == nullptr) {
        SKIP("Set GDL_NATIVE_REFERENCE_DIR to a fresh CRITTER export for comparison");
    }
    usize compared = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (toLowerAscii(entry.path().extension().string()) != ".wad") {
            continue;
        }
        const auto name = normalizeAssetName(entry.path().stem().string());
        CAPTURE(name);
        const auto source = formats::parseCritterWad(readFile(entry.path()));
        const auto legacy = std::filesystem::path(reference) / "critter" / (name + ".json");
        REQUIRE(std::filesystem::exists(legacy));
        for (usize i = 0; i < source.types.size(); ++i) {
            CAPTURE(i);
            CritterData native;
            CritterData exported;
            REQUIRE(native.load(entry.path(), i));
            REQUIRE(exported.load(legacy, i));
            equivalent(native, exported);
            ++compared;
        }
    }
    REQUIRE(compared > 0);
}

TEST_CASE("native boss archives bind fighters and child heads without exported assets",
          "[native-assets][native-critter][assets][combatant]") {
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    AnimationSet shared;
    REQUIRE(shared.load(root / "WEAPONS"));
    const auto ringIndex = shared.find("EXPRING");
    REQUIRE(ringIndex.has_value());
    const auto& ring = shared.tree(*ringIndex).sequences.front();
    const f32 ringLife =
        static_cast<f32>(ring.frames * ring.frameRate) * AnimationPlayer::kRateUnit;
    for (const auto* name : {"LICH", "CHIMERA", "GARM"}) {
        CAPTURE(name);
        REQUIRE_FALSE(std::filesystem::exists(root / "critter" / (std::string(name) + ".json")));
        const auto directory = root / "MONSTERS" / name;
        for (const auto* manifest : {"objects.json", "textures.json", "animations.json"}) {
            REQUIRE_FALSE(std::filesystem::exists(directory / manifest));
        }
        test::FakeRenderDevice device;
        CombatantAssets assets;
        CombatantDefinition definition;
        definition.name = name;
        definition.kind = CombatantKind::Boss;
        REQUIRE(assets.load(device, root, definition, 'G'));
        REQUIRE(assets.body.bound());
        REQUIRE(assets.tree != nullptr);
        // The animation hierarchy also holds invisible markers and transform-only nodes.
        const auto drawableNodes = std::ranges::count_if(assets.tree->nodes, [](const auto& node) {
            const bool hasFrames = std::ranges::any_of(
                node.objectFrames, [](const auto& frames) { return !frames.object.empty(); });
            return node.name != "DUMMY" && node.name != "NULL1" &&
                   (!node.object.empty() || hasFrames);
        });
        REQUIRE(assets.body.nodeCount() == static_cast<usize>(drawableNodes));
        REQUIRE_FALSE(assets.data.moves().empty());
        REQUIRE_FALSE(assets.tree->sequences.empty());
        REQUIRE(assets.effectLifetimes.at("EXPRING") == ringLife);
        if (std::string_view(name) == "CHIMERA") {
            REQUIRE(assets.children.size() == 3);
            for (const auto& child : assets.children) {
                REQUIRE(child.loaded());
                REQUIRE(child.parentIndex() == 0);
                REQUIRE(assets.tree->findNode(child.rootNode()).has_value());
                REQUIRE(child.meter().shown);
            }
        } else {
            REQUIRE(assets.children.empty());
        }
        assets.body.setFrame(0, 0);
        assets.body.draw(device, Mat4{1}, Mat4{1});
        REQUIRE_FALSE(device.draws.empty());
        assets.clear();
        REQUIRE_FALSE(assets.body.bound());
        REQUIRE(assets.children.empty());
        REQUIRE(assets.effectLifetimes.empty());
    }
}

} // namespace
