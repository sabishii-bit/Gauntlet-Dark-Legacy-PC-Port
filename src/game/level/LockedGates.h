#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/LocalBinding.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/LocalState.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/level/Chests.h"
#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"

namespace gdl::game {

/** What a gate did this update. */
struct GateEvent {
    enum class Kind : u8 { Unlocked, Refused };
    Kind kind = Kind::Unlocked;
    usize gate = 0;
    usize visitor = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's locked gates, worked the way the original works them: each is a gate item that
 * stands shut across the way until someone against it has a key to spend; the key spent, it
 * bars nobody from that moment (PlayerCollideItem's door blocks only an unspent one), while
 * a shot still stops on it until it has swung open (fn_8005F0F4's contact).
 */
class LockedGates : public LocalState<LockedGates> {
public:
    LockedGates() = default;
    LockedGates(const LockedGates&) = delete;
    LockedGates& operator=(const LockedGates&) = delete;
    LockedGates(LockedGates&&) noexcept = default;
    LockedGates& operator=(LockedGates&&) noexcept = default;
    ~LockedGates() = default;
    static constexpr s32 kShut = 0; ///< the figure's sequences
    static constexpr s32 kOpening = 1;
    static constexpr s32 kOpen = 2;
    static constexpr f32 kRefusalSeconds = 2.5f;

    /** One gate. */
    struct Gate {
        s32 instance = -1;
        s32 subtype = 0;
        s32 state = kShut;
        s32 openingTicks = 0;
        f32 refusalLeft = 0.0f;
        s32 minPlayers = 0;
        bool shown = true;
        bool blocksPassage = true; ///< floor-only triangle lists are not key-operated barriers
        ItemFigure figure;
        Obstacle box;
        ItemSupport support;
        bool operator==(const Gate&) const = default;
        template <class Visitor> friend void visitNativeState(Visitor& visitor, const Gate& value) {
            visitor.template fields<11>(value);
        }
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
              const WorldCollision* collision);
    void clear();
    void syncFloors();
    usize size() const { return m_sim.gates.size(); }
    const Gate& gate(usize index) const { return m_sim.gates[index]; }
    /** The authored opening cue for this realm and gate material/style. */
    static std::string_view openingSound(s32 realm, s32 subtype);
    void setPlayerCount(s32 players);

    std::vector<GateEvent> update(s32 ticks, f32 seconds, std::span<const ChestVisitor> party);
    /** The developer console opens a shut gate without a key: it swings open as a key would
     * have it. False when it is not shut. */
    bool unlock(usize index);
    /** The developer console shuts a gate again, barring the way until a key is spent. False
     * when it is shut already. */
    bool lock(usize index);
    /** The boxes of the gates that still bar the way. */
    std::vector<Obstacle> obstacles() const;
    /** What stops a shot: a door still shut or swinging (fn_8005F0F4's contact). */
    std::vector<Obstacle> shotStops() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f) const;
    void capturePresentation();
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    struct Simulation {
        std::vector<Gate> gates;
        bool operator==(const Simulation&) const = default;
    } m_sim;

    friend class LocalState<LockedGates>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Simulation;
    struct LocalChildren {};
    static constexpr auto localParts() { return local::parts(local::own()); }
    const LocalValues& localValues() const { return m_sim; }
    LocalValues& localValues() { return m_sim; }
    /** Writes the schema, what the gates were bound to and `state`. */
    bool digestLocalValues(NativeStateDigest& digest, const LocalValues& state) const;
    const WorldCollision* m_collision = nullptr;
    const WorldLayout* m_layout = nullptr; ///< borrowed immutable authored roster
    LocalBinding m_localBinding;
};

} // namespace gdl::game
