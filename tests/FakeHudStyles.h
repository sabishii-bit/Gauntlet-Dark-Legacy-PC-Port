#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"

#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/players/PlayerControls.h"

namespace gdl::test {

/** One of the calls a HUD look takes. */
enum class FakeHudPart : u8 {
    Prepare,
    Status,
    RuneMeter,
    Hourglass,
    BossMeters,
    Selectors,
    Help,
    Browse,
};

/** A call a fake look took. */
struct FakeHudCall {
    std::string look; ///< the id of the look called
    FakeHudPart part = FakeHudPart::Prepare;
    s32 seat = -1; ///< a browse's seat; -1 for the rest
    bool operator==(const FakeHudCall&) const = default;
};

/** What fake looks did, kept apart from them so a test reads it after they have gone. */
struct FakeHudRecord {
    std::vector<FakeHudCall> calls; ///< every call taken, in turn
    std::vector<std::string> made;  ///< the id of each look made, in turn
    std::vector<u32> letGo;         ///< for each look made, its place among those let go; 0: held
    u32 departures = 0;             ///< how many looks have been let go

    /** How many calls look `look` took of `part`. */
    usize count(std::string_view look, FakeHudPart part) const {
        return static_cast<usize>(std::ranges::count_if(calls, [&](const FakeHudCall& call) {
            return call.look == look && call.part == part;
        }));
    }
    /** The ids of the looks let go, in the order they went. */
    std::vector<std::string> destroyed() const {
        std::vector<std::string> ids(departures);
        for (usize look = 0; look < made.size(); ++look) {
            if (letGo[look] != 0) {
                ids[letGo[look] - 1] = made[look];
            }
        }
        return ids;
    }
};

/** A HUD look that draws nothing and tells `record` of every call it takes; the part
 * `throwsIn` names throws once told, as a failing look's would. */
class FakeHudPainter final : public game::HudPainter {
public:
    static constexpr f32 kStatusTop = 280.0f; ///< its panels' top, other than Dark Legacy's

    FakeHudPainter(std::string id, FakeHudRecord& record)
        : m_id(std::move(id)), m_record(record), m_made(record.made.size()) {
        m_record.made.push_back(m_id);
        m_record.letGo.push_back(0);
    }
    // Its going allocates nothing, as a destructor must not throw.
    ~FakeHudPainter() override { m_record.letGo[m_made] = ++m_record.departures; }
    GDL_NON_COPYABLE_NON_MOVABLE(FakeHudPainter);

    std::string_view id() const override { return m_id; }
    f32 statusTop() const override { return kStatusTop; }
    void prepare(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::Prepare);
    }
    void drawStatus(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::Status);
    }
    void drawRuneMeter(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::RuneMeter);
    }
    void drawHourglass(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::Hourglass);
    }
    void drawBossMeters(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::BossMeters);
    }
    void drawSelectors(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::Selectors);
    }
    void drawHelp(game::HudFrame& /*frame*/, const game::HudView& /*view*/) override {
        take(FakeHudPart::Help);
    }
    void browse(s32 seat, const game::SelectorInput& /*presses*/) override {
        take(FakeHudPart::Browse, seat);
    }

    std::optional<FakeHudPart> throwsIn; ///< the call that throws

private:
    void take(FakeHudPart part, s32 seat = -1) {
        m_record.calls.push_back({m_id, part, seat});
        if (throwsIn == part) {
            throw std::runtime_error("the fake look broke");
        }
    }

    std::string m_id;
    FakeHudRecord& m_record;
    usize m_made; ///< its place among the looks `m_record` saw made
};

/** The HUD looks a test's installation offers: `offered` as it stands, each ask counted. Every
 * open is kept with the party it was given; it borrows STATIC's `borrowed` picture from Dark
 * Legacy's art when one is named, then makes a FakeHudPainter reporting to `record`, throwing
 * in `throwsIn`, unless the id is one of `failing`. */
class FakeHudStyles final : public game::HudStyles {
public:
    std::span<const game::HudStyleOffer> offers() override {
        ++asks;
        return offered;
    }
    std::unique_ptr<game::HudPainter> open(std::string_view id, RenderDevice& /*device*/,
                                           game::DarkLegacyHudArt& darkLegacy,
                                           std::span<const game::HudMember> party) override {
        opened.emplace_back(id);
        parties.emplace_back(party.begin(), party.end());
        if (!borrowed.empty()) {
            darkLegacy.staticArt(borrowed);
        }
        if (std::ranges::find(failing, id) != failing.end()) {
            return nullptr;
        }
        auto painter = std::make_unique<FakeHudPainter>(std::string{id}, record);
        painter->throwsIn = throwsIn;
        return painter;
    }

    std::vector<game::HudStyleOffer> offered; ///< usable or not, in the order offered
    std::vector<std::string> failing;         ///< the ids whose looks do not open
    std::optional<FakeHudPart> throwsIn;      ///< where the looks opened from now on throw
    std::string borrowed;                     ///< what each opening borrows; nothing when empty
    FakeHudRecord record;                     ///< what the looks opened did, together
    std::vector<std::string> opened;          ///< each id asked for, in turn
    std::vector<std::vector<game::HudMember>> parties; ///< the party each opening was given
    u32 asks = 0;
};

} // namespace gdl::test
