#pragma once

#include <array>
#include <deque>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/widgets/MenuInput.h"

namespace gdl::game {
/** Host-authored setup; no filenames, disk slots or guest-supplied rewards. */
struct ShopSetup {
    u32 source = 0;
    u32 destination = 0;
    bool expanded = false;
    u8 fallen = 0;
    std::array<s32, 3> maxima{};
    std::array<std::array<s32, 3>, 4> totals{};
    bool ending = false; ///< host-confirmed defeat; local WDATA determines the ending movie
    bool valid() const;
};
struct ShopStep {
    u64 tick = 0;
    std::array<MenuInput, 4> inputs;
    std::array<s32, 4> potions{};
    std::array<s32, 4> focus{-1, -1, -1, -1};
    // 255 denotes held-only input, with no fresh command to validate.
    std::array<u8, 4> observedPhase{255, 255, 255, 255};
    std::array<u16, 4> observedCursor{};
};
/** Reliable, bounded menu stream. Only the host orders transactions and chooses
 * random potion results. Peers reproduce presentation from that committed stream;
 * final authoritative profiles are sent by the ordinary travel checkpoint.
 * Commands authenticate seat, per-epoch grant and exact sequence before mutation. */
class ShopSync {
public:
    enum class Admission : u8 { Accepted, Stale, WrongPeer, Invalid };
    struct Delivery {
        u8 peer = 0;
        std::vector<u8> bytes;
    };
    static constexpr usize kBudget = 256;
    /** A seat's presses the host may hold waiting: ten seconds of one a tick. */
    static constexpr usize kHeldPresses = 600;
    static bool recognizes(std::span<const u8> bytes);
    void begin(u64 epoch, u8 local, const std::array<u8, 4>& owners,
               const std::array<u32, 4>& grants, const ShopSetup* setup = nullptr);
    Admission receive(u8 peer, std::span<const u8> bytes);
    bool sample(u8 seat, const MenuInput& input, s32 focus = -1, u8 observedPhase = 0,
                usize observedCursor = 0);
    std::optional<ShopStep> advance();
    bool publish(const ShopStep& step);
    std::optional<ShopStep> take();
    /** On a guest: the host's steps received and not yet taken. */
    usize held() const { return m_steps.size(); }
    bool finish();
    bool finished() const;
    /** A dropped peer holds the shop no longer: it counts as finished. */
    void suspend(u8 peer);
    const ShopSetup* setup() const { return m_setup ? &*m_setup : nullptr; }
    const std::deque<Delivery>& outgoing() const { return m_outgoing; }
    void sent() { m_outgoing.pop_front(); }
    u64 tick() const { return m_tick; }

private:
    bool broadcast(const std::vector<u8>& bytes);
    u64 m_epoch = 0;
    u8 m_local = 0;
    std::array<u8, 4> m_owners{};
    std::array<u32, 4> m_grants{};
    std::optional<ShopSetup> m_setup;
    std::array<u64, 4> m_sent{};
    std::array<u64, 4> m_received{};
    std::array<u16, 4> m_last{};
    std::array<u16, 4> m_held{};
    struct Command {
        u16 bits = 0;
        s32 focus = -1;
        u8 phase = 0;
        u16 cursor = 0;
    };
    /** Queues a seat's command on the host. One without a press folds into a press-free one
     * still waiting, since only the latest pointer and held buttons matter; false when too
     * many presses wait. */
    bool hold(usize seat, const Command& command);
    std::array<std::deque<Command>, 4> m_inputs;
    std::array<bool, 5> m_finished{};
    std::deque<ShopStep> m_steps;
    std::deque<Delivery> m_outgoing;
    u64 m_tick = 0;
    std::mt19937 m_random{std::random_device{}()};
};
} // namespace gdl::game
