#include "expansions/n64/hud/LegendsN64Plaque.h"

#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/level/LevelCatalog.h"

namespace gdl::game {
namespace {

/** An exit's code: its level's byte, and its realm's above it (e00aae08). */
constexpr u32 kLevelBits = 8;
constexpr u32 kByte = 0xff;

/** The realms Dark Legacy's scrolls name, from one. */
constexpr s32 kNamedRealms = 11;

// FUN_e0058620's transparency steps. A plaque is made at kMade. While its seat stands on its
// exit it clears by kClearing a step while past kClearest, so resting at kResting. Once the seat
// leaves, it clouds over by kClouding a step until past kLetGo, when it lets the exit go; with
// no exit to take its place it clouds on by kGoing a step, and goes once past kGoneAfter.
constexpr u8 kMade = 0xdf;
constexpr u8 kClearing = 0x20;
constexpr u8 kClearest = 0x20;
constexpr u8 kResting = 0x1f;
constexpr u8 kClouding = 0x40;
constexpr u8 kLetGo = 0xbe;
constexpr u8 kGoing = 0x20;
constexpr u8 kGoneAfter = 0xde;

/** The code a seat's exit is answered by: 0 off every exit, and for an exit back to the tower. */
u32 codeOf(std::optional<u32> wanted, const LevelRef* destination) {
    if (!wanted || (destination != nullptr && destination->isTower())) {
        return 0;
    }
    return *wanted;
}

/** `text` in capitals. */
std::string capitals(std::string_view text) {
    std::string upper(text);
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return upper;
}

/** A plaque made for exit `code`, which leads to `destination` when it is Dark Legacy's. */
LegendsN64Plaque::Shown made(u32 code, const LevelRef* destination, u8 transparency) {
    LegendsN64Plaque::Shown shown;
    shown.code = code;
    shown.transparency = transparency;
    if (destination != nullptr) {
        shown.darkLegacy = true;
        shown.realmId = destination->realmId;
        shown.title = capitals(destination->title);
    }
    return shown;
}

} // namespace

std::string legendsN64PlaqueArt(u32 code) {
    return std::format("{}{:02x}{:02x}", kLegendsN64PlaquePrefix, (code >> kLevelBits) & kByte,
                       code & kByte);
}

std::optional<std::string> darkLegacyRealmNameText(s32 realmId) {
    if (realmId < 1 || realmId > kNamedRealms) {
        return std::nullopt;
    }
    return std::format("hud.realm.{}", realmId);
}

void LegendsN64Plaque::step(std::optional<u32> wanted, const LevelRef* destination, bool pictured) {
    const u32 code = codeOf(wanted, destination);
    if (code == m_code) {
        // On the exit it answers to, the plaque clears to rest; off every exit, with the exit
        // let go, it clouds over and goes. An exit without a picture has nothing to fade.
        if (!m_shown) {
            return;
        }
        if (m_code != 0) {
            if (m_transparency > kClearest) {
                m_transparency = static_cast<u8>(m_transparency - kClearing);
            }
        } else if (m_transparency > kGoneAfter) {
            m_shown.reset();
            return;
        } else {
            m_transparency = static_cast<u8>(m_transparency + kGoing);
        }
        m_shown->transparency = m_transparency;
        return;
    }
    if (m_code != 0) {
        // Off its exit, or on another, it clouds over until it lets its exit go.
        if (m_transparency > kLetGo) {
            m_code = 0;
            return;
        }
        m_transparency = static_cast<u8>(m_transparency + kClouding);
        if (m_shown) {
            m_shown->transparency = m_transparency;
        }
        return;
    }
    // Another exit answered: its plaque takes the place of any still clouding over.
    m_code = code;
    m_transparency = kMade;
    m_shown.reset();
    if (pictured) {
        m_shown = made(code, destination, kMade);
    }
}

void LegendsN64Plaque::snap(std::optional<u32> wanted, const LevelRef* destination, bool pictured) {
    m_code = codeOf(wanted, destination);
    m_shown.reset();
    m_transparency = 0;
    if (m_code == 0) {
        return;
    }
    // At rest; an exit without a picture keeps the byte it was answered with.
    m_transparency = pictured ? kResting : kMade;
    if (pictured) {
        m_shown = made(m_code, destination, kResting);
    }
}

std::optional<u32> LegendsN64Plaque::answering() const {
    return m_code != 0 ? std::optional<u32>{m_code} : std::nullopt;
}

} // namespace gdl::game
