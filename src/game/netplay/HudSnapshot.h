#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/netplay/InputCommand.h"

namespace gdl::game {
/** Display-only values. No inventory operations, save records, paths or asset names
 * cross this boundary. A missing seat draws the ordinary empty status card. */
struct HudPowerup {
    f32 strength = 0;
    s32 kind = 0;
    f32 charge = 0;
    u32 flags = 0;
    bool on = false;
    bool valid() const;
};
struct HudTurbo {
    f32 fill = 0;
    Color front = Color::black();
    Color back = Color::black();
    u32 glow = 0;
    s32 gleam = -1;
};
struct HudPlayer {
    std::string name;
    s32 character = 0;
    s32 color = 0;
    s32 level = 1;
    s32 gold = 0;
    s32 health = 0;
    s32 keys = 0;
    s32 potions = 0;
    s32 potionKind = 0;
    u32 runes = 0;
    u32 bossKeys = 0;
    bool inTower = false;
    bool towerPrompt = false;
    bool keysShown = false;
    std::optional<HudTurbo> turbo;
    std::optional<HudPowerup> usage;
    std::optional<HudPowerup> selection;
    s32 labelY = 0;
    bool valid() const;
};
/** Fixed artwork identities, resolved only against the already loaded HUD resources. */
enum class HudCardKind : u8 {
    Crystal,
    Key,
    KeyRing,
    Magic,
    Meat,
    BadMeat,
    Fruit,
    BadFruit,
    Gold,
    Junk,
    Specials,
    Runestone,
    Legend,
    GoldenIcon,
    Coin,
    Count
};
enum class HudCountKind : u8 {
    Orange,
    Red,
    Purple,
    Blue,
    Green,
    Yellow,
    White,
    Black,
    Fangs,
    Feathers,
    Claws,
    Minotaur,
    Falconess,
    Jackal,
    Tigress,
    Ogre,
    Unicorn,
    Medusa,
    Hyena,
    Sumner,
    Count
};
struct HudCard {
    u32 seat = 0;
    HudCardKind kind = HudCardKind::Crystal;
    s32 y = 384;
    bool valid() const;
};
struct HudCount {
    HudCountKind kind = HudCountKind::Orange;
    s32 count = 0;
    s32 total = 0;
    bool valid() const;
};
struct HudBossBar {
    bool visible = false;
    bool frozen = false;
    std::array<s32, 2> widths{};
    bool valid() const;
};
/** Absolute presentation values, not start events or client-side timers. Text is
 * resolved from the trusted scene/language resources, never supplied by packets. */
struct HudScreen {
    bool cinematicBars = false;
    bool gameOver = false;
    f32 transitionOpacity = 0;
    f32 titleScale = 0;
    u32 gameOverLetters = 0;
    bool valid() const;
};
struct HudHourglass {
    f32 elapsed = 1;
    s32 fallingFrame = -1;
    bool valid() const;
};
struct HudScroll {
    u32 message = 0;
    u32 page = 0;
    s32 burnFrame = -1;
    u32 promptAlpha = 0;
    bool valid() const;
};
struct HudHelp {
    s32 id = 0;
    s32 player = -1;
    s32 number = -1;
    s32 character = -1;
    bool pojo = false;
    std::optional<Vec3> anchor;
    bool valid() const;
};
struct HudSnapshot {
    static constexpr usize kMaxCards = 24;
    static constexpr usize kMaxBossBars = 3;
    bool visible = true;
    HudScreen screen;
    std::optional<HudHourglass> hourglass;
    std::optional<f32> runeFill;
    std::optional<HudScroll> scroll;
    std::optional<HudHelp> help;
    std::array<std::optional<HudPlayer>, InputCommand::kSeats> players;
    std::vector<HudCard> cards; ///< Preserves host submission order, including overlapping cards.
    std::array<std::optional<HudCount>, InputCommand::kSeats> counts;
    std::vector<HudBossBar> bossBars;
    bool valid() const;
};
class HudPacket {
public:
    static constexpr usize kHeaderBytes = 104;
    static constexpr usize kSeatBytes = 116;
    static constexpr usize kCardBytes = 12;
    static constexpr usize kCountBytes = 12;
    static constexpr usize kBossBarBytes = 12;
    static constexpr usize kMaxBytes =
        kHeaderBytes + InputCommand::kSeats * (kSeatBytes + kCountBytes) +
        HudSnapshot::kMaxCards * kCardBytes + HudSnapshot::kMaxBossBars * kBossBarBytes;
    static std::optional<std::vector<u8>> encode(const HudSnapshot& snapshot);
    static std::optional<HudSnapshot> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
