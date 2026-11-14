#include "game/level/CodePlacedSpecials.h"

#include <array>

#include "game/combat/PowerupFlags.h"

namespace gdl::game {
namespace {

/** A row's kind, as gOverlayPUPParams names it. */
struct SpecialKind {
    std::string_view name;
    u32 flag = 0;
};

constexpr SpecialKind kHandOfDeath{"HANDOFDEATHPUP", powerup::kHandOfDeath};
constexpr SpecialKind kHealthVamp{"HEALTHVAMPPUP", powerup::kHealthVamp};
constexpr SpecialKind kMikey{"MIKEYPUP", powerup::kMikey};

constexpr CodePlacedSpecial row(std::string_view level, const SpecialKind& kind, f32 x, f32 y,
                                f32 z) {
    return {level, kind.name, kind.flag, Vec3{x, y, z}};
}

constexpr std::array kSpecials{row("G3", kHandOfDeath, 99.9f, -20.0f, -293.6f),
                               row("B3", kMikey, 72.3f, 35.3f, -13.5f),
                               row("B4", kHealthVamp, 1.2f, 1.0f, 25.3f),
                               row("B5", kMikey, 67.3f, 5.8f, -186.5f),
                               row("A1", kHandOfDeath, 50.9f, 0.0f, 20.6f),
                               row("A3", kHealthVamp, 79.6f, 43.6f, -111.3f),
                               row("A4", kMikey, 0.6f, -16.1f, -34.2f),
                               row("K1", kMikey, 57.8f, 0.0f, 21.8f),
                               row("K2", kHandOfDeath, -21.8f, 49.6f, 71.7f),
                               row("K3", kHealthVamp, -116.0f, 35.3f, -378.6f),
                               row("K4", kMikey, -77.2f, 3.0f, 46.0f),
                               row("K4", kHealthVamp, -201.4f, -16.0f, 101.7f),
                               row("D1", kHealthVamp, 2.7f, 14.6f, 95.2f),
                               row("D1", kMikey, 111.8f, 12.2f, 8.5f),
                               row("D2", kHealthVamp, 32.0f, 18.0f, -87.3f),
                               row("D2", kHandOfDeath, 86.8f, 6.4f, -47.5f),
                               row("D3", kMikey, -126.9f, 69.3f, -17.9f),
                               row("D3", kMikey, 138.7f, 65.5f, 20.0f),
                               row("D4", kHealthVamp, -151.2f, 8.4f, -75.1f),
                               row("D4", kMikey, -53.5f, 5.0f, -147.1f),
                               row("D4", kHandOfDeath, 97.4f, -18.8f, -178.0f),
                               row("C1", kHealthVamp, 6.5f, -12.1f, 12.7f),
                               row("C3", kMikey, -64.7f, 30.0f, 58.5f),
                               row("C4", kHandOfDeath, -112.3f, -10.0f, -142.9f),
                               row("C4", kMikey, -135.3f, 0.0f, -26.7f),
                               row("C4", kHandOfDeath, -30.5f, 0.0f, -13.4f),
                               row("C4", kHealthVamp, -42.5f, 0.0f, -27.2f),
                               row("I1", kMikey, 167.4f, 4.2f, 71.9f),
                               row("I1", kHandOfDeath, 295.5f, 10.0f, -188.8f),
                               row("I2", kHealthVamp, -46.2f, 35.0f, 83.0f),
                               row("I4", kHealthVamp, -17.9f, -56.3f, 15.1f),
                               row("J1", kMikey, -5.8f, 0.0f, -19.2f),
                               row("J1", kHandOfDeath, -89.8f, 0.0f, -10.6f),
                               row("J2", kHandOfDeath, 80.9f, 1.5f, 48.0f),
                               row("J2", kMikey, -69.8f, 0.1f, 144.8f),
                               row("J2", kHealthVamp, -158.8f, 28.0f, 111.4f),
                               row("J3", kHandOfDeath, 80.9f, 55.5f, -68.6f),
                               row("J4", kMikey, 133.5f, 49.4f, 92.1f),
                               row("J4", kHealthVamp, 190.0f, 38.6f, 174.9f)};

/** The level record keeps four letters of its name (gCurLevel + 8). */
constexpr usize kNameLetters = 4;

} // namespace

std::span<const CodePlacedSpecial> codePlacedSpecials() {
    return kSpecials;
}

bool inLevel(std::string_view level, std::string_view code) {
    const std::string_view name = level.substr(0, kNameLetters);
    return !name.empty() && code.starts_with(name);
}

} // namespace gdl::game
