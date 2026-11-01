#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/CombatRollbackCheckpoint.h"
#include "game/enemies/OpponentCombatCheckpoint.h"

namespace gdl::game {

/** The complete party/combat and ordinary-opponent adapter supplement. The
 * enclosing snapshot supplies the matching OpponentState exactly once. Resource
 * IDs and native binding descriptions resolve only against preloaded trusted
 * owners; decoding never loads artwork or writes character saves. */
class CombatStatePacket {
public:
    struct State {
        CombatRollbackCheckpoint::State combat;
        OpponentCombatCheckpoint::State adapter;
        /** Catalog-bound opaque stage supplement. The receiver must decode it
         * against its locally prebound stage before accepting a replay seed. */
        std::optional<std::vector<u8>> stage;
        bool valid() const;
        /** Cross-owner links only, after independent trusted leaf validation.
         * Read-only native observations may include boss adapter values that
         * are deliberately outside this packet's ordinary restore domain. */
        bool references() const;
        bool operator==(const State&) const = default;
    };

    static constexpr u32 kVersion = 8;
    static constexpr usize kHeaderBytes = 16;
    static constexpr usize kMaxBytes = static_cast<const usize>(512 * 1024);
    /** Shared budgets for these explicit fields. Complete participant leaves
     * retain their own bounded codec and also count toward kMaxBytes. */
    static constexpr usize kMaxRecords = 8192;
    static constexpr usize kMaxEffects = 512;
    static constexpr usize kMaxBanks = 256;
    static constexpr usize kMaxString = 4096;
    static constexpr usize kMaxStringBytes = static_cast<const usize>(32 * 1024);

    /** All fields are encoded, or the whole supplement is refused. These bounds
     * are transport limits, not changes to native pool capacities. */
    static std::optional<std::vector<u8>> encode(const State& state);
    /** SHA-256 of the canonical body with no opaque Stage. Optional normalization
     * elides only draw-overwritten ItemFigure caches. This is NOT validation;
     * incoming state still requires complete native prepareRestore. */
    static std::optional<std::array<u8, 32>>
    canonicalDigest(const CombatRollbackCheckpoint::State& combat,
                    const OpponentCombatCheckpoint::State& adapter,
                    bool presentationIndependent = false);
    static std::optional<State> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
