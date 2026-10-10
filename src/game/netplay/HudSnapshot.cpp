#include "game/netplay/HudSnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
void color(std::vector<u8>& bytes, Color value) {
    word(bytes,
         u32{value.r} | (u32{value.g} << 8U) | (u32{value.b} << 16U) | (u32{value.a} << 24U));
}
Color color(ByteReader& reader) {
    const auto value = reader.readU32();
    return {static_cast<u8>(value), static_cast<u8>(value >> 8U), static_cast<u8>(value >> 16U),
            static_cast<u8>(value >> 24U)};
}
void powerup(std::vector<u8>& bytes, const std::optional<HudPowerup>& value) {
    if (!value) {
        bytes.insert(bytes.end(), 20, 0);
        return;
    }
    real(bytes, value->strength);
    word(bytes, static_cast<u32>(value->kind));
    real(bytes, value->charge);
    word(bytes, value->flags);
    word(bytes, value->on ? 1 : 0);
}
bool powerup(ByteReader& reader, std::optional<HudPowerup>& value, bool present) {
    if (!present) {
        const auto padding = reader.readBytes(20);
        return std::ranges::all_of(padding, [](u8 byte) { return byte == 0; });
    }
    HudPowerup result;
    result.strength = real(reader);
    result.kind = reader.readS32();
    result.charge = real(reader);
    result.flags = reader.readU32();
    const auto on = reader.readU32();
    if (on > 1) {
        return false;
    }
    result.on = on != 0;
    value = result;
    return true;
}
} // namespace
bool HudPowerup::valid() const {
    return kind >= 5 && kind <= 9 && bounded(strength, -1'000'000, 1'000'000) &&
           bounded(charge, -1'000'000, 1'000'000);
}
bool HudPlayer::valid() const {
    if (name.size() > 6 ||
        !std::ranges::all_of(name, [](unsigned char c) { return c >= 32 && c <= 126; }) ||
        character < 0 || character > 16 || color < 0 || color > 3 || level < 1 || level > 99 ||
        gold < 0 || gold > 1'000'000 || health < 0 || health > 1'000'000 || keys < 0 || keys > 9 ||
        potions < 0 || potions > 9 || potionKind < 0 || potionKind > 4 || runes > 65535 ||
        bossKeys > 65535 || (towerPrompt && !inTower) ||
        (inTower && (health != 0 || turbo || usage)) ||
        (usage && (!usage->valid() || !usage->on || usage->strength == 0)) ||
        (selection && !selection->valid()) ||
        (selection ? labelY < 0 || labelY > 512 : labelY != 0)) {
        return false;
    }
    return !turbo || (bounded(turbo->fill, 0, 1) && turbo->glow <= 255 && turbo->gleam >= -1 &&
                      turbo->gleam < 5);
}
bool HudCard::valid() const {
    return seat < InputCommand::kSeats && kind < HudCardKind::Count && y >= 304 && y < 400;
}
bool HudCount::valid() const {
    // Completed crystal records can be negative; do not reinterpret them as unsigned counts.
    return kind < HudCountKind::Count && count >= -1'000'000 && count <= 1'000'000 && total >= 0 &&
           total <= 1'000'000;
}
bool HudBossBar::valid() const {
    return std::ranges::all_of(widths, [](s32 width) { return width >= 0 && width <= 256; });
}
bool HudScreen::valid() const {
    return bounded(transitionOpacity, 0, 1) && bounded(titleScale, 0, 2) &&
           (gameOver ? !cinematicBars && transitionOpacity == 0 && titleScale == 0 &&
                           gameOverLetters <= 22
                     : gameOverLetters == 0);
}
bool HudHourglass::valid() const {
    // The renderer also checks the index against its preloaded native sand cycle.
    return bounded(elapsed, 0, 1) && fallingFrame >= -1 && fallingFrame < 4096;
}
bool HudScroll::valid() const {
    // Resource validation also checks the message and page against the native table.
    return message < 4096 && page < 256 && burnFrame >= -1 && burnFrame < 21 && promptAlpha <= 255;
}
bool HudHelp::valid() const {
    return id >= 0 && id <= 150 && player >= -1 && player < 4 && number >= -1 &&
           number <= 1'000'000 && character >= -1 && character <= 16 &&
           (!anchor || (bounded(anchor->x, -1'000'000, 1'000'000) &&
                        bounded(anchor->y, -1'000'000, 1'000'000) &&
                        bounded(anchor->z, -1'000'000, 1'000'000)));
}
bool HudSnapshot::valid() const {
    return screen.valid() && !(visible && (screen.cinematicBars || screen.gameOver)) &&
           (!hourglass || hourglass->valid()) && (!runeFill || bounded(*runeFill, 0, 1)) &&
           (!scroll || scroll->valid()) && (!help || help->valid()) && cards.size() <= kMaxCards &&
           bossBars.size() <= kMaxBossBars &&
           std::ranges::all_of(players,
                               [](const auto& player) { return !player || player->valid(); }) &&
           std::ranges::all_of(cards, &HudCard::valid) &&
           std::ranges::all_of(counts,
                               [](const auto& count) { return !count || count->valid(); }) &&
           std::ranges::all_of(bossBars, &HudBossBar::valid);
}
std::optional<std::vector<u8>> HudPacket::encode(const HudSnapshot& snapshot) {
    if (!snapshot.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kMaxBytes);
    word(bytes, fourcc("GDHU"));
    u32 flags = snapshot.visible ? 1 : 0;
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if (snapshot.players[seat]) {
            flags |= 2U << seat;
        }
    }
    word(bytes, flags);
    word(bytes, static_cast<u32>(snapshot.cards.size()));
    u32 countMask = 0;
    for (usize seat = 0; seat < snapshot.counts.size(); ++seat) {
        if (snapshot.counts[seat]) {
            countMask |= 1U << seat;
        }
    }
    word(bytes, countMask);
    word(bytes, static_cast<u32>(snapshot.bossBars.size()));
    word(bytes, (snapshot.screen.cinematicBars ? 1U : 0U) | (snapshot.screen.gameOver ? 2U : 0U));
    real(bytes, snapshot.screen.transitionOpacity);
    real(bytes, snapshot.screen.titleScale);
    word(bytes, snapshot.screen.gameOverLetters);
    word(bytes, (snapshot.runeFill ? 1U : 0U) | (snapshot.hourglass ? 2U : 0U));
    real(bytes, snapshot.runeFill.value_or(0));
    real(bytes, snapshot.hourglass ? snapshot.hourglass->elapsed : 0);
    word(bytes, snapshot.hourglass ? static_cast<u32>(snapshot.hourglass->fallingFrame) : 0);
    word(bytes, (snapshot.scroll ? 1U : 0U) | (snapshot.help ? 2U : 0U));
    if (const auto& scroll = snapshot.scroll) {
        word(bytes, scroll->message);
        word(bytes, scroll->page);
        word(bytes, static_cast<u32>(scroll->burnFrame));
        word(bytes, scroll->promptAlpha);
    } else {
        bytes.insert(bytes.end(), 16, 0);
    }
    if (const auto& help = snapshot.help) {
        word(bytes, static_cast<u32>(help->id));
        word(bytes, static_cast<u32>(help->player));
        word(bytes, static_cast<u32>(help->number));
        word(bytes, static_cast<u32>(help->character));
        word(bytes, (help->pojo ? 1U : 0U) | (help->anchor ? 2U : 0U));
        const Vec3 anchor = help->anchor.value_or(Vec3{0});
        real(bytes, anchor.x);
        real(bytes, anchor.y);
        real(bytes, anchor.z);
    } else {
        bytes.insert(bytes.end(), 32, 0);
    }
    for (const auto& player : snapshot.players) {
        if (!player) {
            continue;
        }
        for (usize i = 0; i < 8; ++i) {
            bytes.push_back(i < player->name.size() ? static_cast<u8>(player->name[i]) : 0);
        }
        for (const auto value :
             {player->character, player->color, player->level, player->gold, player->health,
              player->keys, player->potions, player->potionKind}) {
            word(bytes, static_cast<u32>(value));
        }
        word(bytes, player->runes);
        word(bytes, player->bossKeys);
        word(bytes, (player->inTower ? 1U : 0U) | (player->towerPrompt ? 2U : 0U) |
                        (player->keysShown ? 4U : 0U) | (player->turbo ? 8U : 0U) |
                        (player->usage ? 16U : 0U) | (player->selection ? 32U : 0U));
        if (const auto& turbo = player->turbo) {
            real(bytes, turbo->fill);
            color(bytes, turbo->front);
            color(bytes, turbo->back);
            word(bytes, turbo->glow);
            word(bytes, static_cast<u32>(turbo->gleam));
        } else {
            bytes.insert(bytes.end(), 20, 0);
        }
        powerup(bytes, player->usage);
        powerup(bytes, player->selection);
        word(bytes, static_cast<u32>(player->labelY));
    }
    for (const auto& card : snapshot.cards) {
        word(bytes, card.seat);
        word(bytes, static_cast<u32>(card.kind));
        word(bytes, static_cast<u32>(card.y));
    }
    for (const auto& count : snapshot.counts) {
        if (count) {
            word(bytes, static_cast<u32>(count->kind));
            word(bytes, static_cast<u32>(count->count));
            word(bytes, static_cast<u32>(count->total));
        }
    }
    for (const auto& bar : snapshot.bossBars) {
        word(bytes, (bar.visible ? 1U : 0U) | (bar.frozen ? 2U : 0U));
        for (const auto width : bar.widths) {
            word(bytes, static_cast<u32>(width));
        }
    }
    return bytes;
}
std::optional<HudSnapshot> HudPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != fourcc("GDHU")) {
        return std::nullopt;
    }
    const auto flags = reader.readU32();
    const usize cards = reader.readU32();
    const auto countMask = reader.readU32();
    const usize bars = reader.readU32();
    if ((flags & ~31U) != 0 || (countMask & ~15U) != 0 || cards > HudSnapshot::kMaxCards ||
        bars > HudSnapshot::kMaxBossBars ||
        bytes.size() != kHeaderBytes + static_cast<usize>(std::popcount(flags >> 1U)) * kSeatBytes +
                            cards * kCardBytes +
                            static_cast<usize>(std::popcount(countMask)) * kCountBytes +
                            bars * kBossBarBytes) {
        return std::nullopt;
    }
    HudSnapshot result;
    result.visible = (flags & 1U) != 0;
    const auto screenFlags = reader.readU32();
    if ((screenFlags & ~3U) != 0) {
        return std::nullopt;
    }
    result.screen.cinematicBars = (screenFlags & 1U) != 0;
    result.screen.gameOver = (screenFlags & 2U) != 0;
    result.screen.transitionOpacity = real(reader);
    result.screen.titleScale = real(reader);
    result.screen.gameOverLetters = reader.readU32();
    const auto meterFlags = reader.readU32();
    const auto rune = reader.readU32();
    const auto elapsed = reader.readU32();
    const auto fallingFrame = reader.readS32();
    if ((meterFlags & ~3U) != 0 || ((meterFlags & 1U) == 0 && rune != 0) ||
        ((meterFlags & 2U) == 0 && (elapsed != 0 || fallingFrame != 0))) {
        return std::nullopt;
    }
    if ((meterFlags & 1U) != 0) {
        result.runeFill = std::bit_cast<f32>(rune);
    }
    if ((meterFlags & 2U) != 0) {
        result.hourglass = HudHourglass{std::bit_cast<f32>(elapsed), fallingFrame};
    }
    const auto overlayFlags = reader.readU32();
    if ((overlayFlags & ~3U) != 0) {
        return std::nullopt;
    }
    if ((overlayFlags & 1U) != 0) {
        const auto message = reader.readU32();
        const auto page = reader.readU32();
        const auto burnFrame = reader.readS32();
        result.scroll = HudScroll{message, page, burnFrame, reader.readU32()};
    } else if (!std::ranges::all_of(reader.readBytes(16), [](u8 byte) { return byte == 0; })) {
        return std::nullopt;
    }
    if ((overlayFlags & 2U) != 0) {
        HudHelp help;
        help.id = reader.readS32();
        help.player = reader.readS32();
        help.number = reader.readS32();
        help.character = reader.readS32();
        const auto helpFlags = reader.readU32();
        const auto x = reader.readU32();
        const auto y = reader.readU32();
        const auto z = reader.readU32();
        if ((helpFlags & ~3U) != 0 || ((helpFlags & 2U) == 0 && (x != 0 || y != 0 || z != 0))) {
            return std::nullopt;
        }
        help.pojo = (helpFlags & 1U) != 0;
        if ((helpFlags & 2U) != 0) {
            help.anchor = Vec3{std::bit_cast<f32>(x), std::bit_cast<f32>(y), std::bit_cast<f32>(z)};
        }
        result.help = help;
    } else if (!std::ranges::all_of(reader.readBytes(32), [](u8 byte) { return byte == 0; })) {
        return std::nullopt;
    }
    for (usize seat = 0; seat < result.players.size(); ++seat) {
        if ((flags & (2U << seat)) == 0) {
            continue;
        }
        HudPlayer player;
        const auto name = reader.readBytes(8);
        const auto end = std::ranges::find(name, u8{0});
        if (!std::all_of(end, name.end(), [](u8 byte) { return byte == 0; })) {
            return std::nullopt;
        }
        for (auto at = name.begin(); at != end; ++at) {
            player.name.push_back(static_cast<char>(*at));
        }
        player.character = reader.readS32();
        player.color = reader.readS32();
        player.level = reader.readS32();
        player.gold = reader.readS32();
        player.health = reader.readS32();
        player.keys = reader.readS32();
        player.potions = reader.readS32();
        player.potionKind = reader.readS32();
        player.runes = reader.readU32();
        player.bossKeys = reader.readU32();
        const auto status = reader.readU32();
        if ((status & ~63U) != 0) {
            return std::nullopt;
        }
        player.inTower = (status & 1U) != 0;
        player.towerPrompt = (status & 2U) != 0;
        player.keysShown = (status & 4U) != 0;
        if ((status & 8U) != 0) {
            HudTurbo turbo;
            turbo.fill = real(reader);
            turbo.front = color(reader);
            turbo.back = color(reader);
            turbo.glow = reader.readU32();
            turbo.gleam = reader.readS32();
            player.turbo = turbo;
        } else if (!std::ranges::all_of(reader.readBytes(20), [](u8 byte) { return byte == 0; })) {
            return std::nullopt;
        }
        if (!powerup(reader, player.usage, (status & 16U) != 0) ||
            !powerup(reader, player.selection, (status & 32U) != 0)) {
            return std::nullopt;
        }
        player.labelY = reader.readS32();
        result.players[seat] = std::move(player);
    }
    for (usize i = 0; i < cards; ++i) {
        const auto seat = reader.readU32();
        const auto kind = reader.readU32();
        if (kind >= static_cast<u32>(HudCardKind::Count)) {
            return std::nullopt;
        }
        result.cards.push_back({seat, static_cast<HudCardKind>(kind), reader.readS32()});
    }
    for (usize seat = 0; seat < result.counts.size(); ++seat) {
        if ((countMask & (1U << seat)) != 0) {
            const auto kind = reader.readU32();
            if (kind >= static_cast<u32>(HudCountKind::Count)) {
                return std::nullopt;
            }
            const auto count = reader.readS32();
            result.counts[seat] =
                HudCount{static_cast<HudCountKind>(kind), count, reader.readS32()};
        }
    }
    for (usize i = 0; i < bars; ++i) {
        const auto state = reader.readU32();
        if ((state & ~3U) != 0) {
            return std::nullopt;
        }
        const auto first = reader.readS32();
        result.bossBars.push_back(
            {(state & 1U) != 0, (state & 2U) != 0, {first, reader.readS32()}});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
} // namespace gdl::game
