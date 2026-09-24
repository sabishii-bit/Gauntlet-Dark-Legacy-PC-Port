#include "game/screens/HelpMessages.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/menu/ScrollBox.h"

namespace gdl::game {

namespace {

constexpr std::array<HelpMessageSpec, 87> kSpecs{{
    {HelpMessages::kDoorNeedsKey, "USEKEYOPENDOOR", "S_USEKEY"},
    {HelpMessages::kChestNeedsKey, "USEKEYOPENCHEST", "S_USEKEY2"},
    {HelpMessages::kPotionsFull, "FULLOFBOMBS", "S_MAGICFULL"},
    {HelpMessages::kKeysFull, "FULLOFKEYS", "S_KEYFULL"},
    {HelpMessages::kNoPotion, "COLLECTMAGICFIRST", "S_COLLECTPOT"},
    {7, "USEMAGIC2", "S_USEMAGIC2"},
    {8, "SAVEKEYS", "S_SAVEKEYS"},
    {15, "EATMEAT", "S_MEATGIVES"},
    {16, "EATFRUIT", "S_FRUITGIVES"},
    {17, "COLLECTGOLD", "S_COLLECTGOLD"},
    {28, "POISONEDFOOD", "S_POISONEDFOOD"},
    {94, "THROWMAGIC", "S_THROWMAGIC"},
    {95, "MAGICSHIELD", "S_SHIELDMAGIC"},
    {HelpMessages::kTrapsHurt, "AVOIDOBJECTS", "S_AVOID"},
    {HelpMessages::kRandomChest, "RANDOMCHEST", "S_SILVER"},
    {HelpMessages::kBarrelsHold, "WOODBARREL", "S_SOMEBARRELS"},
    // message.c's descriptor IDs and text indices, resolved against ENGLISH's names.
    {32, "SPEEDUP", "S_XSPEED"},
    {33, "MAGICUP", "S_XMAGIC"},
    {35, "INVULNERABILITY", "S_INVULVOX"},
    {36, "INVISIBILITY", "S_INVISVOX"},
    {37, "THREEWAYS", "S_3WAYSHOTVOX"},
    {38, "REFLECTSHOT", "S_REFLECTVOX"},
    {39, "SEETHRU", "S_XRAYVOX"},
    {40, "REDAMULET", "S_FIREAMVOX"},
    {41, "BLUEAMULET", "S_LGHTNGAMVOX"},
    {42, "YELAMULET", "S_LIGHTAMVOX"},
    {43, "GREENAMULET", "S_ACIDAMVOX"},
    {47, "FIVEWAYS", "S_5WAYSHOTVOX"},
    {48, "SUPERSHOT", "S_SUPERVOX"},
    {49, "HALO", "S_ANTIDEATHVOX"},
    {51, "TIMESTOP", "S_STOPPEDVOX"},
    {52, "REFLECTARMOR", "S_REFLECTSHVOX"},
    {53, "LEVITATION", "S_LEVVOX"},
    {54, "INVULNERABILITY", "S_INVULVOX"},
    {81, "FIREBREATHE", "S_FIREBRVOX"},
    {82, "ACIDBREATHE", "S_ACIDBRVOX"},
    {83, "ELECBREATHE", "S_LGHTNGBRVOX"},
    {84, "PHOENIX", "S_PHOENIXVOX"},
    {86, "HAMMERMSG", "S_HAMMERVOX"},
    {87, "RAPIDFIREMSG", "S_RAPIDFIREVOX"},
    {88, "GROWTHMSG", "S_GROWTHVOX"},
    {89, "SHRINKMSG", "S_SHRINKVOX"},
    {91, "FIRESHIELDMSG", "S_FIREWALLSHVOX"},
    {92, "ELECSHIELDMSG", "S_LGHTNGSHVOX"},
    {93, "POJOMSG", "S_POJOVOX", HelpRepeat::Always},
    {98, "MASKMSG", "S_MASKVOX"},
    {99, "HORNSMSG", "S_HORNSVOX"},
    {100, "GAUNTLETMSG", "S_GAUNTLETVOX"},
    {113, "TURBOBOOST", "S_TURBOBOOST"},
    {132, "GASMASK", "S_GASMASK"},
    {148, "MIKEY", "S_PICKUPCRYST", HelpRepeat::OnceForAll, -1, 50, false, true},
    {149, "HANDOFDEATH", "S_PICKUPCRYST", HelpRepeat::OnceForAll, -1, 50, false, true},
    {150, "HEALTHVAMP", "S_PICKUPCRYST", HelpRepeat::OnceForAll, -1, 50, false, true},
    // The classes' turbo attacks by name: the lesser, then the greater.
    {57, "WAR_TURBO", "S_FIREARC", HelpRepeat::OncePerSession, 1, 60, true},
    {58, "WAR_TURBO", "S_PLASMATRAIL", HelpRepeat::OncePerSession, 2, 70, true},
    {60, "VAL_TURBO", "S_MULTIBLADE", HelpRepeat::OncePerSession, 1, 60, true},
    {61, "VAL_TURBO", "S_SKYLANCE", HelpRepeat::OncePerSession, 2, 70, true},
    {63, "WIZ_TURBO", "S_ROCKSHOWER", HelpRepeat::OncePerSession, 1, 60, true},
    {64, "WIZ_TURBO", "S_DEMONSKULL", HelpRepeat::OncePerSession, 2, 70, true},
    {66, "ARC_TURBO", "S_DOUBLEBOW", HelpRepeat::OncePerSession, 1, 60, true},
    {67, "ARC_TURBO", "S_BFG", HelpRepeat::OncePerSession, 2, 70, true},
    {69, "DWF_TURBO", "S_TURB_DWF", HelpRepeat::OncePerSession, 1, 60, true},
    {70, "DWF_TURBO", "S_TURC_DWF", HelpRepeat::OncePerSession, 2, 70, true},
    {72, "KNI_TURBO", "S_TURB_KNI", HelpRepeat::OncePerSession, 1, 60, true},
    {73, "KNI_TURBO", "S_TURC_KNI", HelpRepeat::OncePerSession, 2, 70, true},
    {75, "SOR_TURBO", "S_TURB_SOR", HelpRepeat::OncePerSession, 1, 60, true},
    {76, "SOR_TURBO", "S_TURC_SOR", HelpRepeat::OncePerSession, 2, 70, true},
    {78, "JES_TURBO", "S_TURB_JES", HelpRepeat::OncePerSession, 1, 60, true},
    {79, "JES_TURBO", "S_TURC_JES", HelpRepeat::OncePerSession, 2, 70, true},
    {HelpMessages::kAlreadyHaveRune, "ALREADYHAVERUNE", "S_ALREADYRUNE", HelpRepeat::Always},
    {HelpMessages::kUseTurbo, "USETURBO", "S_USETURBO"},
    // The legend items by name as they are found, one to a realm: the castle's scimitar to
    // the sky's javelin. The underworld's and the battlefield's have no item; their rows keep
    // the realm numbering.
    {HelpMessages::kFirstLegendName + 1, "LEGEND_ITEMS000", "S_SCIMITARVOX"},
    {HelpMessages::kFirstLegendName + 2, "LEGEND_ITEMS001", "S_ICEAXEVOX"},
    {HelpMessages::kFirstLegendName + 3, "LEGEND_ITEMS002", "S_LAMPVOX"},
    {HelpMessages::kFirstLegendName + 4, "LEGEND_ITEMS003", "S_BELLOWSVOX"},
    {HelpMessages::kFirstLegendName + 5, "LEGEND_ITEMS004", "S_SAVIORVOX"},
    {HelpMessages::kFirstLegendName + 6, "LEGEND_ITEMS005", "S_SAVIORVOX"},
    {HelpMessages::kFirstLegendName + 7, "LEGEND_ITEMS006", "S_BOOKVOX"},
    {HelpMessages::kFirstLegendName + 8, "LEGEND_ITEMS007", "S_SAVIORVOX"},
    {HelpMessages::kFirstLegendName + 9, "LEGEND_ITEMS008", "S_PARCHVOX"},
    {HelpMessages::kFirstLegendName + 10, "LEGEND_ITEMS009", "S_LANTERNVOX"},
    {HelpMessages::kFirstLegendName + 11, "LEGEND_ITEMS010", "S_JAVELINVOX"},
    {HelpMessages::kHealthFull, "HEALTHFULL", "S_HEALTHFULL", HelpRepeat::OncePerPlayer},
    {HelpMessages::kLevelUp, "LEVELUP", "S_GAINEDLEVEL", HelpRepeat::Always},
    {HelpMessages::kBlastsDestroy, "EXPDESTROY", "S_EXPDSTITMS"},
    {HelpMessages::kGasSpoils, "GASPOISON", "S_GASFOODBAD"},
    {HelpMessages::kChestsExplode, "CHESTSEXPL", "S_CHESTSEXPL"},
}};

/** The original's ink for players one to four: dark yellow, blue, red and green. */
constexpr std::array<Color, 4> kInks{Color::rgba(0x1F, 0x1F, 0x00), Color::rgba(0x00, 0x00, 0x1F),
                                     Color::rgba(0x1F, 0x00, 0x00), Color::rgba(0x00, 0x1F, 0x00)};

} // namespace

const HelpMessageSpec* HelpMessages::specOf(s32 id) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kSpecs, id, &HelpMessageSpec::id);
    return found != kSpecs.end() ? &*found : nullptr;
}

Color HelpMessages::inkOf(s32 player) {
    return player >= 0 && static_cast<usize>(player) < kInks.size()
               ? kInks[static_cast<usize>(player)]
               : ScrollBox::kTextColor;
}

bool HelpMessages::gameplayTip(s32 id) {
    return (id >= kDoorNeedsKey && id <= 28) || id == 94 || id == 95 || id == kUseTurbo ||
           (id >= kBlastsDestroy && id <= kChestsExplode);
}

HelpMessages::VoiceLead HelpMessages::voiceLead(s32 id, bool multiplayer) {
    if (id == 93) {
        return VoiceLead::PlayerName;
    }
    if (id == kLevelUp || id == 89) {
        return VoiceLead::PlayerHas;
    }
    if (multiplayer) {
        // Retail does not prefix shield, gas-mask, turbo or crystal announcements.
        static constexpr std::array kNamed{32,  33,  35,  36,  37,  38,  39,  40,  41,  42,
                                           43,  47,  48,  49,  51,  52,  53,  54,  81,  82,
                                           83,  84,  86,  87,  88,  98,  99,  100, 114, 115,
                                           116, 117, 118, 119, 120, 121, 122, 123, 124};
        if (std::ranges::find(kNamed, id) != kNamed.end()) {
            return VoiceLead::PlayerHas;
        }
    }
    return VoiceLead::None;
}

void HelpMessages::clear() {
    m_lines.clear();
    m_id = -1;
    m_priority = 0;
    m_ticksLeft = 0;
    m_pauseLeft = 0;
    m_posted = 0;
}

const HelpMessageSpec* HelpMessages::post(s32 id, s32 player, std::span<const HelpReader> party,
                                          s32 number) {
    const HelpMessageSpec* spec = specOf(id);
    if (spec == nullptr || m_strings == nullptr) {
        return nullptr;
    }
    // One at a time, unless it outranks what is up; the pause between them is the lessons'.
    const bool lesson = id <= 28 || id == 44 || id == 45 || id == 55 || id == 80;
    if ((showing() && m_priority >= spec->priority) || (lesson && m_pauseLeft > 0)) {
        return nullptr;
    }
    const auto sawIt = [id](const HelpReader& reader) {
        return reader.seen != nullptr && std::ranges::binary_search(*reader.seen, id);
    };
    const auto heardIt = [id](const HelpReader& reader) {
        return reader.heard != nullptr && std::ranges::binary_search(*reader.heard, id);
    };
    const auto concerns = [&](const HelpReader& reader) {
        return spec->repeat != HelpRepeat::OncePerPlayer || reader.player == player;
    };
    // Told once: a player's own message until that player has seen it, a lesson until
    // everyone playing has, a session's until anyone playing has heard it since loading.
    bool wanted = spec->repeat == HelpRepeat::Always;
    if (spec->repeat == HelpRepeat::OncePerSession) {
        wanted = std::ranges::none_of(party, heardIt);
    } else if (!wanted) {
        wanted = std::ranges::any_of(
            party, [&](const HelpReader& reader) { return concerns(reader) && !sawIt(reader); });
    }
    const auto message = m_strings->find(spec->text);
    if (!wanted || !message.has_value()) {
        return nullptr;
    }
    const MessageInfo& info = m_strings->message(*message);
    // The strings keep a message's lines as its pages.
    std::vector<std::string> lines;
    for (usize page = 0; page < info.pages.size(); ++page) {
        if (spec->line >= 0 && page != static_cast<usize>(spec->line)) {
            continue;
        }
        for (std::string& line : ScrollBox::splitLines(info.pages[page])) {
            // A number the message asks for ("LEVEL %d") is filled in.
            if (const auto at = line.find("%d"); at != std::string::npos && number >= 0) {
                line.replace(at, 2, std::to_string(number));
            }
            lines.push_back(std::move(line));
        }
    }
    if (lines.empty()) {
        return nullptr;
    }
    m_lines = std::move(lines);
    const auto note = [id](std::vector<s32>* list) {
        if (list != nullptr && !std::ranges::binary_search(*list, id)) {
            list->insert(std::ranges::upper_bound(*list, id), id);
        }
    };
    for (const HelpReader& reader : party) {
        if (concerns(reader)) {
            note(reader.seen);
            note(reader.heard);
        }
    }
    m_id = id;
    m_priority = spec->priority;
    m_player = player;
    m_ticksLeft = static_cast<s32>(m_lines.size()) * kTicksPerLine + kTicksOver;
    return spec;
}

void HelpMessages::update(s32 ticks) {
    m_pauseLeft = std::max(m_pauseLeft - ticks, 0);
    if (m_ticksLeft <= 0) {
        return;
    }
    m_ticksLeft -= ticks;
    if (m_ticksLeft <= 0) {
        m_ticksLeft = 0;
        m_priority = 0;
        m_pauseLeft = kPauses[std::min(m_posted, kPauses.size() - 1)];
        ++m_posted;
    }
}

Rect HelpMessages::areaFor(const TextPainter& text, const Vec2& head) const {
    s32 widest = 0;
    for (const std::string& line : m_lines) {
        widest = std::max(widest, text.measure(line));
    }
    const auto width = static_cast<f32>(widest + kMarginAcross);
    const auto height =
        static_cast<f32>(static_cast<s32>(m_lines.size()) * text.lineHeight() + kMarginDown);
    const f32 left =
        std::clamp(head.x - width * 0.5f, 0.0f, std::max(static_cast<f32>(kWidest) - width, 0.0f));
    const f32 top = std::clamp(head.y - static_cast<f32>(kAboveHead) - height * 0.5f, 2.0f,
                               std::max(static_cast<f32>(kLowest) - height, 2.0f));
    return Rect{left, top, width, height};
}

void HelpMessages::draw(Canvas& canvas, const TextPainter& text, const Texture* scroll,
                        const Vec2& head) const {
    if (!showing() || !text.ready()) {
        return;
    }
    const Rect area = areaFor(text, head);
    if (scroll != nullptr) {
        canvas.draw(*scroll, area, Color::white().withAlpha(kScrollAlpha));
    }
    TextStyle style;
    style.color = inkOf(m_player);
    const auto centre = static_cast<s32>(area.x + area.width * 0.5f);
    s32 y = static_cast<s32>(area.y) + kMarginDown / 2;
    for (const std::string& line : m_lines) {
        text.draw(canvas, -centre, y, line, style);
        y += text.lineHeight();
    }
}

} // namespace gdl::game
