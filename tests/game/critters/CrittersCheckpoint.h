#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/critters/CombatantCheckpoint.h"
#include "game/critters/Critters.h"

namespace gdl::game {
/** Full post-phase population checkpoint. All referenced stocks must already be
 * loaded on the destination. Loading another stock invalidates this wrapper;
 * normal spawning/recycling from that fixed catalog remains rewindable. The
 * population, archives and external stage bindings outlive this wrapper. */
class CrittersCheckpoint {
public:
    static constexpr usize kMaxStocks = 64;
    struct Bank {
        u32 id = 0;
        CombatantKind kind = CombatantKind::Unknown;
        std::string name, tree;
        TextureAnimator::Checkpoint clock;
        bool operator==(const Bank&) const = default;
    };
    struct State {
        EnemyScales scales;
        char realm = 'G';
        std::string gargoyleForm;
        LookoutRoute lookouts;
        std::vector<Bank> banks;
        std::array<CombatantCheckpoint::State, Critters::kMost> actors;
        CombatantCheckpoint::Events events;
        bool valid() const;
        bool operator==(const State&) const = default;

    private:
        friend class CrittersCheckpoint;
        bool validAggregate() const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class CrittersCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit CrittersCheckpoint(Critters& owner);
    ~CrittersCheckpoint();
    CrittersCheckpoint(const CrittersCheckpoint&) = delete;
    CrittersCheckpoint& operator=(const CrittersCheckpoint&) = delete;
    CrittersCheckpoint(CrittersCheckpoint&&) = delete;
    CrittersCheckpoint& operator=(CrittersCheckpoint&&) = delete;
    std::optional<State> checkpoint() const;
    /** Canonical load-bound stock IDs shared with launched attacks; no actor
     * slot, live incarnation, or allocation order participates in this roster. */
    std::span<const CombatantCheckpoint::Resource> resources() const { return m_resources; }
    std::optional<Prepared> prepareRestore(const State& /*state*/) const;
    bool canCommit(const Prepared& /*value*/) const;
    /** Jointly preflights every actor and bank before any mutation; no allocation,
     * resource binding or callback occurs during the synchronous commit. */
    bool commitRestore(Prepared&& /*value*/);
    bool restore(const State& /*state*/);

private:
    bool bound() const;
    bool matches(const State& /*state*/) const;
    Critters& m_owner;
    std::shared_ptr<const u8> m_binding;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    const RenderDevice* m_device = nullptr;
    const WorldCollision* m_collision = nullptr;
    const HazardSurfaces* m_hazards = nullptr;
    const Texture* m_hitFlash = nullptr;
    std::vector<CombatantCheckpoint::Resource> m_resources;
    std::vector<Bank> m_banks;
    EnemyScales m_scales;
    char m_realm = 'G';
    std::string m_gargoyleForm;
    LookoutRoute m_lookouts;
    std::array<std::unique_ptr<CombatantCheckpoint>, Critters::kMost> m_actors;
};
} // namespace gdl::game
