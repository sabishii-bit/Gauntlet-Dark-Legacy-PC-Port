#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/audio/SceneSoundEvents.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/level/Traps.h"
#include "game/players/PlayerControls.h"
#include "game/players/TurboMeter.h"
#include "game/rollback/FrameEventLedger.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr u64 kEpoch = 73;
constexpr f64 kStep = 1.0 / 60;
constexpr u64 kWait = 900;         ///< recorded ticks a sound is waited for at most
constexpr u32 kHarm = 31;          ///< a member's hurt, by its kind
constexpr u32 kMotion = 32;        ///< the selector's and the turbo's answers to a member's step
constexpr u32 kAction = 33;        ///< what a member's movement set off, by its action
constexpr u32 kNamed = 34;         ///< an announcement by a member's name, or a run's praise
constexpr s32 kAboveWarning = 155; ///< a blow of ten takes this past the badly-hurt warning

/** A recording G1 scene of two whose party order is not their seat order. */
struct SceneSounds {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    GameContext context;
    PlayScene scene;
    u64 tick = 0;

    SceneSounds() {
        REQUIRE(catalog.load(root));
        REQUIRE(world.load(device, root, *catalog.byName("G1")));
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        CharacterSave save;
        save.name = "SOUND";
        save.progress().health = 5000;
        const std::array party{PartyMember{3, save}, PartyMember{1, save}};
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{18.8f, 0.2f, 0.7f};
        REQUIRE(scene.open(device, context, world, party, options));
        const auto entering = [&] {
            return scene.spawning() || scene.animator(3)->entering() ||
                   scene.animator(1)->entering();
        };
        for (s32 i = 0; i < 480 && entering(); ++i) {
            REQUIRE(scene.update(kStep, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(entering());
        REQUIRE(scene.beginRecordedAudio(kEpoch));
    }
    PlayerRuntime& player(s32 seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable scene fixture
        return *const_cast<PlayerRuntime*>(scene.runtime(seat));
    }
    /** One recorded tick's sounds. */
    std::vector<SceneSoundEvents::Ledger::Event> record(const PlayScene::Inputs& inputs) {
        const auto result = scene.recordStep(kEpoch, ++tick, kStep, inputs);
        REQUIRE(result.outcome == PlayOutcome::Running);
        REQUIRE(result.audio);
        return result.audio->finite.events;
    }
    /** The sounds among `events` from the scene's own member scopes, at `seat` or at every
     * seat, each as "namespace entity generation seat cue". */
    static std::vector<std::string> members(std::span<const SceneSoundEvents::Ledger::Event> events,
                                            std::optional<u8> seat = std::nullopt) {
        std::vector<std::string> sources;
        for (const auto& event : events) {
            const FrameEventEmitter& emitter = event.id.emitter;
            if (emitter.nameSpace >= kHarm && emitter.nameSpace <= kNamed &&
                (!seat.has_value() || emitter.seat == *seat)) {
                sources.push_back(std::format("{} {} {} {} cue {}", emitter.nameSpace,
                                              emitter.entity, emitter.generation, emitter.seat,
                                              event.id.cue));
            }
        }
        return sources;
    }
    /** Recorded ticks of `inputs` until one sounds from `nameSpace` at `seat`; that tick's
     * sounds from the member scopes at that seat. */
    std::vector<std::string> until(const PlayScene::Inputs& inputs, u32 nameSpace, u8 seat) {
        for (u64 attempt = 0; attempt < kWait; ++attempt) {
            const auto events = record(inputs);
            if (std::ranges::any_of(events, [&](const auto& event) {
                    return event.id.emitter.nameSpace == nameSpace && event.id.emitter.seat == seat;
                })) {
                return members(events, seat);
            }
        }
        FAIL("nothing sounded from the scope waited for");
        return {};
    }
};

TEST_CASE("the scene's own sounds keep their emitters", "[scene-sound-identity][assets]") {
    auto f = std::make_unique<SceneSounds>();
    // The powerup selector opens for each member under its own seat: the step's first answer.
    PlayScene::Inputs selecting{};
    for (const s32 seat : {3, 1}) {
        f->player(seat).actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                                     armor::kResistAcid, 0, 60);
        selecting[static_cast<usize>(seat)].selector.up = true;
    }
    CHECK(SceneSounds::members(f->record(selecting)) ==
          std::vector<std::string>{"32 0 1 3 cue 1", "32 0 1 1 cue 1"});
    // A turbo attack's cry sounds as the turbo goes on: the step's second answer.
    f->player(3).turbo.add(TurboMeter::kFullCost);
    PlayScene::Inputs turbo{};
    turbo[3].turbo = true;
    turbo[3].attack = true;
    turbo[3].attackPressed = true;
    turbo[3].turboAttackPressed = true;
    CHECK(f->until(turbo, kMotion, 3) == std::vector<std::string>{"32 0 1 3 cue 2"});
    // A footfall is an action of the member's movement, under its seat rather than its place
    // in the party: the second member sits at seat 1, the first at seat 3.
    PlayScene::Inputs walking{};
    walking[1].move = MoveInput{Vec2{0, 1}, 1};
    CHECK(f->until(walking, kAction, 1) == std::vector<std::string>{"33 0 1 1 cue 11"});
    PlayScene::Inputs leading{};
    leading[3].move = MoveInput{Vec2{0, 1}, 1};
    CHECK(f->until(leading, kAction, 3) == std::vector<std::string>{"33 0 1 3 cue 11"});
    // The blow that takes a member past the warning names it from inside the hurt; the
    // spikes' next blow is a groan, by the hurt's kind.
    const auto& traps = f->scene.traps();
    usize spike = traps.size();
    for (usize index = 0; index < traps.size() && spike == traps.size(); ++index) {
        const auto& trap = traps.trap(index);
        if (trap.shown && !trap.gone && trap.subtype == Traps::kSpikes && trap.damage > 0) {
            spike = index;
        }
    }
    REQUIRE(spike < traps.size());
    f->player(3).actor.place(traps.trap(spike).figure.position());
    f->player(3).actor.save().progress().health = kAboveWarning;
    CHECK(f->until({}, kNamed, 3) == std::vector<std::string>{"34 0 1 3 cue 1", "34 0 1 3 cue 1"});
    CHECK(f->until({}, kHarm, 3) == std::vector<std::string>{"31 0 1 3 cue 3"});
}
} // namespace
