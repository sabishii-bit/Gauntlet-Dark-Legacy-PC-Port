#include "game/netplay/NativePlaySetup.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string_view>
#include <utility>

#include "engine/io/Sha256.h"

namespace gdl::game {
namespace {
bool text(std::string_view value, usize limit) {
    return !value.empty() && value.size() <= limit &&
           std::ranges::all_of(value, [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/** Explicit little-endian fields, length-prefixed byte strings and IEEE float
 * bits. No native object representation, std::hash, paths or pointer identities. */
class Digest {
public:
    void word(u32 value) {
        std::array<u8, 4> bytes{};
        for (u32 i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<u8>(value >> (i * 8));
        }
        m_hash.update(bytes);
    }
    void boolean(bool value) { word(value ? 1U : 0U); }
    void real(f32 value) { word(std::bit_cast<u32>(value)); }
    void vector(const Vec3& value) {
        real(value.x);
        real(value.y);
        real(value.z);
    }
    void string(std::string_view value) {
        word(static_cast<u32>(value.size()));
        // byte view of a trivially copyable buffer
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        m_hash.update({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    void bytes(std::span<const u8> value) { m_hash.update(value); }
    std::string finish() const { return m_hash.hex(); }

private:
    Sha256 m_hash;
};
} // namespace

std::unique_ptr<NativePlaySetup> NativePlaySetup::create(const GameConfig& local,
                                                         const RoomSettings& room,
                                                         const MatchParty& profiles,
                                                         const PlayOptions& options,
                                                         const Contract& contract) {
    const u8 knownRules = contract.mechanics != nullptr ? contract.mechanics->knownRoomBits() : 0;
    if (!room.valid() || (room.mechanics & ~knownRules) != 0 || contract.scene == 0 ||
        contract.scene > 65535 || !text(contract.build, kMaxIdentifierBytes) ||
        !text(contract.content, kMaxIdentifierBytes) || !text(contract.data, kMaxIdentifierBytes) ||
        !text(local.text.language, kMaxLanguageBytes) || options.items.size() > kMaxDroppedItems ||
        (options.position && !finite(*options.position)) ||
        (options.yaw && !std::isfinite(*options.yaw)) ||
        !std::ranges::all_of(options.items, [](const DroppedItem& item) {
            return text(item.name, kMaxItemNameBytes) && finite(item.position);
        })) {
        return nullptr;
    }
    std::array<std::optional<std::vector<u8>>, InputCommand::kSeats> encoded;
    usize count = 0;
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        if (profiles[seat]) {
            encoded[seat] = CharacterProfilePacket::encode(*profiles[seat]);
            if (!encoded[seat]) {
                return nullptr;
            }
            ++count;
        }
    }
    if (count == 0 || count > room.maxPlayers) {
        return nullptr;
    }

    auto result = std::unique_ptr<NativePlaySetup>(new NativePlaySetup);
    result->m_config = local;
    result->m_config.timing.tickRate = kTickRate;
    result->m_config.difficulty.level = DifficultyConfig::kNames[room.difficulty];
    result->m_config.multiplayer.mode = static_cast<MultiplayerMode>(room.friendlyFire);
    result->m_config.multiplayer.soloCamera = room.camera == 1;
    applyRoomRules(contract.mechanics, room.mechanics, result->m_config.game);
    result->m_config.combat.autoMelee = contract.autoMelee;
    result->m_config.combat.autoActivateItems = contract.autoActivateItems;
    result->m_config.game.expandedShop = contract.expandedShop;
    result->m_config.combat.autoAim = contract.autoAim;
    result->m_options = options;
    result->m_scene = contract.scene;
    result->m_party.reserve(count);

    Digest digest;
    digest.string("gdl-native-play-setup");
    digest.word(kDigestVersion);
    digest.string(contract.build);
    digest.string(contract.content);
    digest.string(contract.data);
    digest.word(contract.scene);
    digest.word(room.maxPlayers);
    digest.word(room.difficulty);
    digest.word(room.friendlyFire);
    digest.word(room.camera);
    digest.word(room.mechanics);
    digest.word(kTickRate);
    digest.boolean(contract.autoMelee);
    digest.boolean(contract.autoActivateItems);
    digest.boolean(contract.expandedShop);
    digest.boolean(contract.autoAim);
    digest.string(result->m_config.text.language);
    digest.real(result->m_stopTimeTotal); // Initial campaign value, never a live-state digest.
    digest.boolean(options.position.has_value());
    if (options.position) {
        digest.vector(*options.position);
    }
    digest.boolean(options.yaw.has_value());
    if (options.yaw) {
        digest.real(*options.yaw);
    }
    digest.boolean(options.welcome.has_value());
    if (options.welcome) {
        digest.boolean(*options.welcome);
    }
    digest.word(static_cast<u32>(options.items.size()));
    for (const auto& item : options.items) {
        digest.string(item.name);
        digest.vector(item.position);
    }
    digest.word(options.arrivalWorld);
    digest.boolean(options.arriving);
    digest.word(static_cast<u32>(profiles.size()));
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        digest.boolean(profiles[seat].has_value());
        if (profiles[seat] && encoded[seat]) {
            digest.bytes(*encoded[seat]);
            PartyMember member;
            member.player = static_cast<s32>(seat);
            member.save = profiles[seat]->gameplayCopy();
            // A new class stores zero health until its first spawn. This is an
            // initial campaign roster, not a continuation's fallen-party state.
            result->m_party.push_back(std::move(member));
        }
    }
    result->m_digest = digest.finish();
    return result;
}
} // namespace gdl::game
