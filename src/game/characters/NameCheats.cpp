#include "game/characters/NameCheats.h"

#include <array>

#include "engine/core/Types.h"

#include "game/characters/Inventory.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
namespace {

// GUNE5D Hidden (80120618), dispatched by set_hidden_player (8007B558).
// NAK069 is deliberately disabled in the ordinary retail table. Debug-menu
// activation chords are not name-entry cheats and are not reproduced here.
// CYBMOS is the remake's own row: the unused Old Thor warrior shipped in PLAYERS/WAR/HIR
// (a complete costume with its WEAP_HOLD), reached by the name the community fixed on.
constexpr std::array<HiddenCostume, 27> kCostumes{{
    {"ICE600", 4, 2, "GEI"}, {"NUD069", 4, 1, "SNM"}, {"STX222", 7, 1, "STK"},
    {"KJH105", 7, 3, "KJH"}, {"PNK666", 7, 0, "PNK"}, {"BAT900", 5, 3, "GEB"},
    {"TAK118", 5, 2, "NIN"}, {"STG333", 5, 3, "STG"}, {"KAO292", 5, 2, "WTR"},
    {"CSS222", 5, 0, "CSS"}, {"RIZ721", 5, 1, "RIZ"}, {"ARV984", 5, 1, "ARV"},
    {"DIB626", 5, 2, "DIB"}, {"SJB964", 5, 1, "SJB"}, {"TWN300", 1, 3, "GET"},
    {"AYA555", 1, 1, "SCH"}, {"CEL721", 1, 3, "CEL"}, {"CAS400", 0, 1, "GEC"},
    {"MTN200", 0, 2, "GEM"}, {"RAT333", 0, 2, "RAT"}, {"GARM99", 2, 0, "GA2"},
    {"GARM00", 2, 3, "GAM"}, {"DES700", 2, 0, "GED"}, {"SKY100", 2, 3, "GEP"},
    {"SUM224", 2, 0, "SUM"}, {"DARTHC", 5, 3, "DCY"}, {"CYBMOS", 0, 1, "HIR"},
}};

struct PowerupCode {
    std::string_view name;
    s32 kind;
    f32 charge;
    u32 flags;
};

// GUNE5D Cheats (801209E4): the powerup rows use duration -1. ALLFUL and
// 10000K are count assignments handled below; both rows of 1ANGEL must run.
constexpr std::array<PowerupCode, 14> kPowerups{{
    {"INVULN", powerup::kArmor, 0, armor::kInvulnerable},
    {"SSHOTS", powerup::kWeapon, -1, damage::kSuperShot},
    {"1ANGEL", powerup::kSpecial, 0, powerup::kLevitation},
    {"1ANGEL", powerup::kArmor, 0, armor::kAntiDeath},
    {"DELTA1", powerup::kSpecial, 0, powerup::kGrowth | powerup::kEnemyShrink},
    {"000000", powerup::kSpecial, 0, powerup::kInvisible},
    {"PEEKIN", powerup::kSpecial, 0, powerup::kXRay},
    {"PURPLE", powerup::kSpecial, 0, powerup::kTurbo},
    {"XSPEED", powerup::kSpecial, 4, powerup::kSpeedBoost},
    {"QCKSHT", powerup::kWeapon, 0, damage::kRapidFire},
    {"MENAGE", powerup::kWeapon, 0, damage::kThreeWayShot},
    {"REFLEX", powerup::kWeapon, 0, damage::kReflect},
    {"NOVATO", powerup::kSpecial, 0, powerup::kStopTime},
    {"MEBERT", powerup::kSpecial, 0, powerup::kPhoenix},
}};

} // namespace

std::span<const HiddenCostume> hiddenCostumes() {
    return kCostumes;
}

const HiddenCostume* hiddenCostume(std::string_view name) {
    // DBRNKR selects the same enabled table row as AYA555.
    if (name == "DBRNKR") {
        name = "AYA555";
    }
    for (const auto& costume : kCostumes) {
        if (costume.name == name) {
            return &costume;
        }
    }
    return nullptr;
}

void restoreNameForm(CharacterSave& save) {
    for (auto& progress : save.classes) {
        restoreNameForm(progress.inventory, save.name);
    }
}

void restoreNameForm(Inventory& inventory, std::string_view name) {
    // Deliberate port behavior: EGG911 is a permanent transformation, not retail's
    // PlayerAddPowerup(-1) slot. Keep all eleven slots available and migrate saves
    // from builds which put the cheat in the selector, even if it was switched off.
    inventory.permanentPojo = name == "EGG911";
    if (!inventory.permanentPojo) {
        return;
    }
    for (auto& slot : inventory.powerups) {
        if (slot.kind == powerup::kSpecial && (slot.flags & powerup::kPojo) != 0) {
            slot.flags &= ~powerup::kPojo;
            if (slot.flags == 0) {
                slot = {};
            }
        }
    }
}

bool applyNameCheats(CharacterSave& save) {
    restoreNameForm(save);
    if (save.name == "EGG911") {
        return true;
    }
    if (const auto* costume = hiddenCostume(save.name)) {
        // SUM224 and change_player(16) both select player_sumner_desc in retail,
        // including its fixed progression, not just the wizard's costume.
        if (costume->directory == "SUM" && save.character == costume->character) {
            // Earlier port saves stored this costume's earned progress under WIZ.
            // Preserve it when moving to SUM, without overwriting a played SUM slot.
            auto& sumner = save.classes[kSumnerClass];
            ClassProgress unused;
            // JSON normalizes an unused slot's -1 promotion marker to level 1.
            unused.promotedLevel = sumner.promotedLevel;
            if (sumner == unused) {
                sumner = save.progress();
                sumner.gold = save.gold;
            }
        }
        save.selectClass(costume->directory == "SUM" ? kSumnerClass : costume->character);
        save.color = costume->color;
        return true;
    }
    if (save.name == "10000K") {
        save.gold = 10000;
        return true;
    }
    Inventory& inventory = save.progress().inventory;
    if (save.name == "ALLFUL") {
        inventory.keys = Inventory::kMostKeys;
        // Raising the count reveals the original slots' initial colour sequence.
        // Existing carried potions retain their colour.
        while (inventory.potions.size() < static_cast<usize>(Inventory::kMostPotions)) {
            inventory.potions.push_back(static_cast<s32>(inventory.potions.size() % 4));
        }
        return true;
    }
    bool recognized = false;
    for (const auto& code : kPowerups) {
        if (code.name == save.name) {
            inventory.addPowerup(code.kind, code.flags, code.charge, -1);
            if (code.kind == powerup::kSpecial && code.flags == powerup::kTurbo) {
                // Deliberate port UX: PURPLE grants active permanent turbo. Ordinary
                // Turbo Boost pickups still wait for the native selector activation.
                for (auto& slot : inventory.powerups) {
                    if (slot.kind == code.kind && slot.flags == code.flags) {
                        slot.on = true;
                    }
                }
            }
            recognized = true;
        }
    }
    return recognized;
}

} // namespace gdl::game
