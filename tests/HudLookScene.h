#pragma once

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"

#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Inventory.h"
#include "game/characters/Party.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/PowerupSelector.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayInput.h"
#include "game/players/PlayerControls.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/PlayScene.h"

namespace gdl::test {

/** When the HUD look scenes' long runs press the selectors. */
struct HudPresses {
    static constexpr u32 kEvery = 20;     ///< ticks between every seat's presses
    static constexpr u32 kOpenTick = 400; ///< the tick the first seat opens its selector
    /** The tick it moves its selection right, once its label has slid in: a selector still
     * sliding in heeds no left or right. */
    static constexpr u32 kSteerTick = 436;
    /** The tick it slides it away again. */
    static constexpr u32 kCloseTick = 440;
    static constexpr u32 kFirstAnchor = 32; ///< the digest tests' earlier anchor
    static constexpr u32 kLastTick = 648;   ///< the runs' last tick, the digest tests' later anchor
};
static_assert(HudPresses::kSteerTick - HudPresses::kOpenTick >
              game::PowerupSelector::kSlide / game::PowerupSelector::kSlidePerTick);
static_assert(HudPresses::kSteerTick < HudPresses::kCloseTick);

/** A powerup that a selector can name, carried for good and not worn. */
inline constexpr game::PowerupSlot kSelectorPowerup{
    .strength = -1.0f, .kind = game::powerup::kSpeed, .charge = 5.0f, .flags = 0, .on = false};

/** One way a selector is pressed. */
enum class Press : u8 { Up, Down, Left, Right };

inline game::SelectorInput pressed(Press way) {
    return {.up = way == Press::Up,
            .down = way == Press::Down,
            .left = way == Press::Left,
            .right = way == Press::Right};
}

/** The native digest tests' inputs at `tick`: still through the entrance, then each seat
 * walking, attacking, swinging hard, guarding and drinking in turn. Each seat's selector is
 * pressed down, left or right every HudPresses::kEvery ticks, a seat a turn on from the one
 * before; the first seat's is opened at kOpenTick, moved right at kSteerTick and closed at
 * kCloseTick. */
inline game::PlayScene::Inputs hudInputsAt(u32 tick) {
    constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    constexpr std::array kTurns{Press::Down, Press::Left, Press::Right};
    constexpr u32 kStill = 360; // the ticks the party stands still through the entrance
    constexpr u32 kWalk = 120;  // the ticks a seat walks one way before turning
    // A seat's round of deeds, each a seat later than the one before: an attack held from the
    // round's start, a hard swing, then the guard; and a potion drunk every kDrinkEvery ticks.
    constexpr u32 kRound = 240;
    constexpr u32 kSeatLag = 30;
    constexpr u32 kAttackTo = 90;
    constexpr u32 kSwingFrom = 120;
    constexpr u32 kSwingTo = 150;
    constexpr u32 kGuardFrom = 180;
    constexpr u32 kGuardTo = 190;
    constexpr u32 kDrinkEvery = 600;
    constexpr u32 kDrinkAt = 300;
    game::PlayScene::Inputs inputs{};
    for (u8 seat = 0; seat < game::PlayScene::kPlayerCount; ++seat) {
        game::PlayInput input;
        if (tick > kStill) {
            const u32 elapsed = tick - kStill - 1;
            const u32 phase = (elapsed + seat * kSeatLag) % kRound;
            const auto direction = kDirections[(elapsed / kWalk + seat) % kDirections.size()];
            input.move = {direction, 1};
            input.aimDirection = {direction.x, 0, -direction.y};
            input.attack = phase < kAttackTo;
            input.attackPressed = phase == 0;
            input.strongAttack = phase >= kSwingFrom && phase < kSwingTo;
            input.turbo = phase >= kGuardFrom && phase < kGuardTo;
            input.defendPressed = phase == kGuardFrom;
            input.usePotion = elapsed % kDrinkEvery == kDrinkAt;
        }
        if (seat == 0 && tick == HudPresses::kOpenTick) {
            input.selector = pressed(Press::Up);
        } else if (seat == 0 && tick == HudPresses::kSteerTick) {
            input.selector = pressed(Press::Right);
        } else if (seat == 0 && tick == HudPresses::kCloseTick) {
            input.selector = pressed(Press::Down);
        } else if (tick % HudPresses::kEvery == 0) {
            input.selector = pressed(kTurns[(tick / HudPresses::kEvery + seat) % kTurns.size()]);
        }
        inputs[seat] =
            game::SessionInputs::playInput(game::SessionInputs::command(input, 1, tick, 1, seat));
    }
    return inputs;
}

/** A look drawn by `inner`, a real one, that tells `record` of every call it passes on, as a
 * FakeHudPainter tells of its own. */
class WatchedHudPainter final : public game::HudPainter {
public:
    WatchedHudPainter(std::unique_ptr<game::HudPainter> inner, FakeHudRecord& record)
        : m_inner(std::move(inner)), m_record(record) {}
    ~WatchedHudPainter() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(WatchedHudPainter);

    std::string_view id() const override { return m_inner->id(); }
    f32 statusTop() const override { return m_inner->statusTop(); }
    void prepare(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::Prepare);
        m_inner->prepare(frame, view);
    }
    void drawStatus(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::Status);
        m_inner->drawStatus(frame, view);
    }
    void drawRuneMeter(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::RuneMeter);
        m_inner->drawRuneMeter(frame, view);
    }
    void drawHourglass(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::Hourglass);
        m_inner->drawHourglass(frame, view);
    }
    void drawBossMeters(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::BossMeters);
        m_inner->drawBossMeters(frame, view);
    }
    void drawSelectors(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::Selectors);
        m_inner->drawSelectors(frame, view);
    }
    void drawHelp(game::HudFrame& frame, const game::HudView& view) override {
        take(FakeHudPart::Help);
        m_inner->drawHelp(frame, view);
    }
    void browse(s32 seat, const game::SelectorInput& presses) override {
        take(FakeHudPart::Browse, seat);
        m_inner->browse(seat, presses);
    }
    void steer(s32 seat, const game::SelectorInput& presses) override {
        take(FakeHudPart::Steer, seat);
        m_inner->steer(seat, presses);
    }

private:
    void take(FakeHudPart part, s32 seat = -1) {
        m_record.calls.push_back({std::string{m_inner->id()}, part, seat});
    }

    std::unique_ptr<game::HudPainter> m_inner;
    FakeHudRecord& m_record;
};

/** A level opened as the native digest tests open theirs, for `seats` players in seat order,
 * its HUD drawn in a look of `styles`, or in Dark Legacy's alone. Skips without the game data.
 */
template <class Styles> struct HudLookScene {
    static constexpr f32 kFrameWidth = 640; ///< the frames' size
    static constexpr f32 kFrameHeight = 448;
    static constexpr s32 kTickRate = 60;
    static constexpr f64 kStep = 1.0 / kTickRate;
    // What each member carries in: more health than any run takes, keys and potions to spend,
    // and a full turbo meter.
    static constexpr s32 kHealth = 4000;
    static constexpr s32 kKeys = 9;
    static constexpr s32 kPotions = 9;
    static constexpr f32 kTurbo = 100;
    static constexpr u32 kMostSettlingTicks = 600; ///< how long a party may take to come in

    std::filesystem::path root =
        assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    FakeRenderDevice device;
    game::GameConfig config;
    game::LevelCatalog catalog;
    StringTable strings;
    game::LevelWorld world;
    game::GameServices context;
    Styles styles; ///< outlives the scene, whose looks it opened
    game::PlayScene scene;
    std::vector<game::PartyMember> party;
    game::PlayOptions options;
    u32 tick = 0;

    explicit HudLookScene(std::string_view stage, s32 seats = 2) {
        assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", stage));
        config.timing.tickRate = kTickRate;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(dataDirectory() / "text", "en"));
        const auto level =
            stage == "L1" ? std::optional{game::LevelRef::tower()} : catalog.byName(stage);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        for (s32 seat = 0; seat < seats; ++seat) {
            game::CharacterSave save;
            save.name = "LOOKS";
            save.character = seat;
            save.color = seat;
            save.progress().health = kHealth;
            save.progress().inventory.addKeys(kKeys);
            save.progress().inventory.addPotions(seat + 1, kPotions);
            party.push_back({seat, save, static_cast<usize>(seat), false, kTurbo});
        }
        options.welcome = false;
    }

    /** Opens the scene with the settings choosing `look`, or with Dark Legacy's look when none
     * is named: then with no catalog, unless `linked`, as the game links its own whatever the
     * settings choose. */
    void open(std::string_view look = {}, bool linked = false) {
        config.display.hudStyle =
            look.empty() ? std::string{game::kDarkLegacyHudStyle} : std::string{look};
        REQUIRE(scene.open(device, context, world, party, options,
                           {.hudStyles = look.empty() && !linked ? nullptr : &styles}));
        tick = 0;
    }
    void step(const game::PlayScene::Inputs& inputs) {
        ++tick;
        REQUIRE(scene.update(kStep, inputs) == game::PlayOutcome::Running);
    }
    bool entering() const {
        if (scene.spawning()) {
            return true;
        }
        for (s32 seat = 0; seat < game::PlayScene::kPlayerCount; ++seat) {
            if (scene.animator(seat) != nullptr && scene.animator(seat)->entering()) {
                return true;
            }
        }
        return false;
    }
    /** Steps with nothing pressed until the party has come in and stands free to act. */
    void settle() {
        for (u32 guard = 0; guard < kMostSettlingTicks && entering(); ++guard) {
            step({});
        }
        REQUIRE_FALSE(entering());
    }
    /** The scene's agreement digest, or its local digest of `saved`; taking it uploads and
     * draws nothing. */
    NativeStateDigest::Digest digest(const game::PlayScene::LocalSnapshot* saved = nullptr) {
        const auto textures = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        const auto draws = device.draws.size();
        const auto value =
            saved != nullptr ? scene.nativeLocalDigest(*saved) : scene.nativeAgreementDigest();
        REQUIRE(value);
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == uploads);
        CHECK(device.draws.size() == draws);
        return *value;
    }
    /** Draws a frame, under the settings when `optionsOpen`. */
    void render(f32 blend = -1.0f, bool optionsOpen = false) {
        device.draws.clear();
        device.bloomDrawOffsets.clear();
        REQUIRE(device.beginFrame());
        scene.render(device, makeScreenProjection(kFrameWidth, kFrameHeight), kFrameWidth,
                     kFrameHeight, optionsOpen, blend);
        device.endFrame();
    }
    /** The vertices the last frame drew over the view, its HUD and the rest of its overlay: the
     * draws after the bloom's, so the settings must have bloom on. */
    std::vector<ImmediateVertex> overlay() const {
        REQUIRE(device.bloomDrawOffsets.size() == 1);
        std::vector<ImmediateVertex> vertices;
        for (const RecordedDraw& draw :
             std::span{device.draws}.subspan(device.bloomDrawOffsets.front())) {
            vertices.insert(vertices.end(), draw.vertices.begin(), draw.vertices.end());
        }
        return vertices;
    }
};

} // namespace gdl::test
