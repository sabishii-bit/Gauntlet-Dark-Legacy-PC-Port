#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/config/GameConfig.h"
#include "game/mechanics/GameplayMechanic.h"
#include "game/mechanics/MechanicCatalog.h"

namespace gdl::game {

/**
 * The one place the base game asks about the mechanics laid over it: a level's instance of
 * every mechanic the build carries, answering each facet for them all. With no mechanic it
 * answers as the original does, so a hook never needs to ask whether one is there.
 *
 * Answers fold in catalog order: the first mechanic with something to say about a blow, a
 * run's quiet or a birth decides it; any may keep a blow from a generator; tunings and body
 * scales multiply; the first marked look rims the swarm and every tint is laid on.
 */
class MechanicsFacade final : public BlowRules,
                              public RunRules,
                              public SwarmRules,
                              public BirthRules,
                              public FallRules,
                              public SwarmAppearance {
public:
    MechanicsFacade() = default;
    ~MechanicsFacade() override;
    GDL_NON_COPYABLE_NON_MOVABLE(MechanicsFacade);

    /** The original's answers, for a hook handed no facade. */
    static const MechanicsFacade& standard();

    /** Builds one of every mechanic in `catalog` for `level`; none for a null catalog. */
    void open(const MechanicCatalog* catalog, const MechanicLevel& level);
    /** Lets every mechanic's presentation go and drops them. */
    void close();
    /** Switches each mechanic as `options` say, off when unsaid. */
    void configure(const GameOptionsConfig& options);
    bool empty() const { return m_members.empty(); }
    /** This level's instance of the mechanic `id`, or null. */
    const GameplayMechanic* find(std::string_view id) const;

    std::optional<RunEffect> runEffect(const BlowReport& blow) const override;
    bool generatorTakes(BlowSource source) const override;
    std::optional<f32> quietWindow(s32 count) const override;
    void step(const SwarmStep& step) override;
    SwarmTuning tuning() const override;
    BodyTraits traitsFor(const BirthRequest& birth, Mt19937& random) const override;
    void fell(const BodyFall& fall, BagDrops& drops) override;
    SwarmLook look(u32 frame) const override;
    f32 bodyScale(s32 affinity) const override;
    void drawWorld(const WorldOverlayView& view);
    void drawHud(const HudOverlayView& view);

    class LocalSnapshot {
        friend class MechanicsFacade;
        const MechanicsFacade* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
        std::vector<std::unique_ptr<MechanicState>> m_states;
        std::vector<u8> m_enabled;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    struct Member {
        const MechanicDescriptor* descriptor = nullptr;
        std::unique_ptr<GameplayMechanic> mechanic;
        bool enabled = false;
    };
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;

    std::vector<Member> m_members;
    std::shared_ptr<const u8> m_binding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
