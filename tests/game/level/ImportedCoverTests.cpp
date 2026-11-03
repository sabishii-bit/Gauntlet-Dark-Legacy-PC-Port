#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/level/CombatCover.h"
#include "game/level/ImportedCover.h"
#include "game/level/ImportedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_base_of_v<CombatCover, ImportedCover>);
static_assert(std::is_final_v<ImportedCover>);

/** One question put to an imported level's arena. */
struct ArenaProbe {
    Vec3 from;
    Vec3 to;
    f32 radius = 0;
};

/** An imported level with nothing of its own to say about cover. */
class PlainLevel : public ImportedLevel {
public:
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
};

/** An imported level whose arena shelters whatever lies past z 4, logging each question. */
class ArenaLevel final : public PlainLevel {
public:
    explicit ArenaLevel(std::vector<ArenaProbe>& probes) : m_probes(probes) {}
    bool arenaBlocksSegment(const Vec3& from, const Vec3& to, f32 radius) const override {
        m_probes.push_back({from, to, radius});
        return to.z > kShelteredPast;
    }

private:
    static constexpr f32 kShelteredPast = 4.0f;
    std::vector<ArenaProbe>& m_probes;
};

TEST_CASE("imported cover asks the level's arena, breath and areas at their own widths",
          "[game][combat-cover][imported-cover]") {
    std::vector<ArenaProbe> probes;
    const ArenaLevel level{probes};
    const ImportedCover importedCover{level};
    const CombatCover& cover = importedCover;
    CHECK(cover.blocksBreath(Vec3{1, 0, 0}, Vec3{0, 0, 5}));
    CHECK_FALSE(cover.blocksBreath(Vec3{1, 0, 0}, Vec3{0, 0, 3}));
    CHECK(cover.blocksArea(Vec3{2, 0, 0}, Vec3{0, 0, 15}));
    CHECK_FALSE(cover.blocksArea(Vec3{2, 0, 0}, Vec3{0, 0, 1}));
    REQUIRE(probes.size() == 4);
    CHECK(probes[0].from == Vec3{1, 0, 0});
    CHECK(probes[0].to == Vec3{0, 0, 5});
    CHECK(probes[0].radius == CombatCover::kBreathProbeRadius);
    CHECK(probes[1].to == Vec3{0, 0, 3});
    CHECK(probes[1].radius == CombatCover::kBreathProbeRadius);
    CHECK(probes[2].from == Vec3{2, 0, 0});
    CHECK(probes[2].to == Vec3{0, 0, 15});
    CHECK(probes[2].radius == CombatCover::kAreaProbeRadius);
    CHECK(probes[3].to == Vec3{0, 0, 1});
    CHECK(probes[3].radius == CombatCover::kAreaProbeRadius);
}

TEST_CASE("an imported level without an arena of its own shelters nobody",
          "[game][combat-cover][imported-cover]") {
    const PlainLevel level{};
    const ImportedCover importedCover{level};
    const CombatCover& cover = importedCover;
    CHECK_FALSE(cover.blocksBreath(Vec3{0}, Vec3{0, 0, 5}));
    CHECK_FALSE(cover.blocksArea(Vec3{0}, Vec3{0, 0, 15}));
}
} // namespace
