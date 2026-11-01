#pragma once

#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/n64/formats/LegendsN64Tower.h"

namespace gdl::game {

/** GDL locomotion against the native Tower sphere/cylinder contact volume.
 * Sweeps, then slides tangentially or refuses the move; never teleports a body
 * out of a volume. A body already inside may move out. Invalid requests retain
 * the old position; an invalid old position is a caller error. */
Vec3 resolveLegendsTowerDoor(const formats::LegendsN64TowerDoor& door, const Vec3& from,
                             const Vec3& to, f32 radius, f32 height);

/** Sumner slides like the native category7 body; category14's podium refuses
 * contact. Continuous sweeps retain GDL traversal and prevent fast tunnelling. */
Vec3 resolveLegendsTowerVolume(const formats::LegendsN64TowerVolume& volume, const Vec3& from,
                               const Vec3& to, f32 radius, f32 height);

/** Native Tower idle decisions only. Presentation supplies sequence completion
 * and a 15-bit random value; this state never reads a renderer clock or mixer. */
class LegendsTowerIdle {
public:
    struct Cue {
        std::string_view sumner;
        std::string_view podium;
    };
    static std::string_view initialSequence() { return "LKDN"; }
    std::optional<Cue> update(bool finished, u16 random15);

private:
    enum class State { Reading, ConjureStart, ConjureLoop };
    State m_state = State::Reading;
    u8 m_conjureLoops = 0;
};

/** Door activation is a progression event, never a proximity heuristic. The
 * opening animation starts at its held first frame and holds its final frame. */
class LegendsTowerDoorMotion {
public:
    enum class State { Closed, Opening, Open };
    explicit LegendsTowerDoorMotion(bool alreadyOpen = false)
        : m_state(alreadyOpen ? State::Open : State::Closed) {}
    bool unlock();
    void update(bool animationFinished);
    State state() const { return m_state; }
    bool held() const { return m_state == State::Closed; }

private:
    State m_state;
};

} // namespace gdl::game
