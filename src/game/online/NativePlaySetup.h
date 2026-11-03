#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "game/config/GameConfig.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/netplay/RoomService.h"
#include "game/rollback/PartyCheckpoint.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/** Owned, canonical starting inputs for a native online campaign. This is not a
 * handshake: the caller must agree the digest and scene roster before opening
 * any scene. Local device bindings, disk authority and outputs stay outside it.
 * Keep this owner alive while the online game's services borrow config() and its
 * scenes stopTimeTotal(), including while a secret level retains its parent scene. */
class NativePlaySetup {
public:
    static constexpr u32 kDigestVersion = 4;
    static constexpr u32 kTickRate = 60;
    static constexpr usize kMaxIdentifierBytes = 256;
    static constexpr usize kMaxLanguageBytes = 32;
    static constexpr usize kMaxDroppedItems = 256;
    static constexpr usize kMaxItemNameBytes = 128;

    struct Contract {
        u32 scene = 0; ///< trusted catalog ID, not a peer-provided path
        bool autoMelee = false;
        bool autoActivateItems = true;
        bool expandedShop = false;
        bool autoAim = true;
        std::string build;   ///< executable/simulation compatibility identifier
        std::string content; ///< native asset tree identifier
        std::string data;    ///< resolved gameplay/text data identifier
        const MechanicCatalog* mechanics = nullptr; ///< what the room's rule bits name
    };

    /** Returns null for invalid or oversized input; never truncates it. The
     * caller supplies the agreed gameplay flags, not this machine's preferences.
     * The copied local language is hashed too: different resolved text is not
     * silently declared equivalent. Identifiers and names are bounded printable
     * ASCII. Position/yaw must be finite; validating the trusted scene ID and
     * authored item names remains the scene loader's job. The room switches the
     * mechanics: a rule bit the contract's catalog does not offer is refused. */
    static std::unique_ptr<NativePlaySetup>
    create(const GameConfig& local, const RoomSettings& room, const MatchParty& profiles,
           const PlayOptions& options, const Contract& contract);

    NativePlaySetup(const NativePlaySetup&) = delete;
    NativePlaySetup& operator=(const NativePlaySetup&) = delete;
    NativePlaySetup(NativePlaySetup&&) = delete;
    NativePlaySetup& operator=(NativePlaySetup&&) = delete;
    ~NativePlaySetup() = default;

    const GameConfig& config() const { return m_config; }
    const PlayOptions& options() const { return m_options; }
    std::span<const PartyMember> party() const { return m_party; }
    const std::string& digest() const { return m_digest; }
    u32 scene() const { return m_scene; }
    f32& stopTimeTotal() { return m_stopTimeTotal; }
    f32 stopTimeTotal() const { return m_stopTimeTotal; }

private:
    NativePlaySetup() = default;

    GameConfig m_config;
    PlayOptions m_options;
    std::vector<PartyMember> m_party;
    std::string m_digest;
    u32 m_scene = 0;
    f32 m_stopTimeTotal = 0;
};
} // namespace gdl::game
