#include "game/screens/ReplicaHud.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"

#include "game/screens/CinematicBars.h"
#include "game/screens/GameOver.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/screens/PartyNames.h"
#include "game/screens/RuneMeter.h"

namespace gdl::game {
namespace {
constexpr std::array<std::string_view, static_cast<usize>(HudCardKind::Count)> kCards{
    "CRYSTAL", "KEY",  "KEY_RING", "MAGIC",     "MEAT",   "BADMEAT",   "FRUIT",  "BADFRUIT",
    "GOLD",    "JUNK", "SPECIALS", "RUNESTONE", "LEGEND", "GOLDNICON", "COINHUD"};
constexpr std::array<std::string_view, static_cast<usize>(HudCountKind::Count)> kCounts{
    "SM_CRYSTAL_ORA", "SM_CRYSTAL_RED", "SM_CRYSTAL_PUR", "SM_CRYSTAL_BLU", "SM_CRYSTAL_GRE",
    "SM_CRYSTAL_YEL", "SM_CRYSTAL_WHI", "SM_CRYSTAL_BLA", "SM_FANGS",       "SM_FEATHERS",
    "SM_CLAWS",       "16_MINCOIN",     "16_FALCOIN",     "16_JACCOIN",     "16_TIGCOIN",
    "16_OGRCOIN",     "16_UNICOIN",     "16_MEDCOIN",     "16_HYECOIN",     "16_SUM"};
static_assert(HudSnapshot::kMaxCards == PickupHud::kMostCards);
static_assert(HudSnapshot::kMaxBossBars == BossMeters::kCapacity);
static_assert(PickupHud::kCardRestY == 304 && PickupHud::kCardEndY == 400);
static_assert(BossMeter::kPieceWidth == 256 && BossMeter::kMostPieces == 2);
static_assert((GameOver::kDurationTicks - GameOver::kVoiceTick) / GameOver::kLetterTicks == 22);
HudPowerup capturePowerup(const PowerupSlot& slot) {
    return {slot.strength, slot.kind, slot.charge, slot.flags, slot.on};
}
PowerupSlot displayedPowerup(const HudPowerup& slot) {
    return {slot.strength, slot.kind, slot.charge, slot.flags, slot.on};
}
LevelMessages::Look scrollLook(const HudScroll& scroll) {
    return {scroll.message, scroll.page, scroll.burnFrame, static_cast<u8>(scroll.promptAlpha)};
}
HelpMessages::Look helpLook(const HudHelp& help) {
    return {help.id, help.player, help.number, {help.character, help.pojo}};
}
} // namespace
HudScreen HudCapture::screen(const TransitionScreen& transition,
                             const LevelArrivalPresentation& arrival, const GameOver& gameOver,
                             bool cinematicBars) {
    if (gameOver.active()) {
        return {false, true, 0, 0, static_cast<u32>(gameOver.letters())};
    }
    return {cinematicBars, false, transition.opacity(), arrival.titleScale(), 0};
}
std::optional<HudSnapshot> HudCapture::capture(std::span<const PlayerRuntime> players,
                                               const PartyHud& hud, bool visible,
                                               const BossMeters* bosses) {
    HudSnapshot result;
    result.visible = visible;
    if (const auto look = hud.help().look()) {
        result.help = HudHelp{look->id,           look->player,
                              look->number,       look->speaker.character,
                              look->speaker.pojo, hud.helpAnchor(players)};
    }
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& runtime : players) {
        const s32 id = runtime.actor.player();
        if (id < 0 || id >= static_cast<s32>(seen.size()) || seen[static_cast<usize>(id)]) {
            return std::nullopt;
        }
        seen[static_cast<usize>(id)] = true;
        if (runtime.departed) {
            continue;
        }
        const auto& selector = hud.selector(id);
        const auto view = PartyHud::status(id, players, &selector);
        HudPlayer player;
        player.name = view.name;
        player.character = view.classIndex;
        player.color = view.color;
        player.level = view.level;
        player.gold = view.gold;
        player.health = view.health;
        player.keys = view.keys;
        player.potions = view.potions;
        player.potionKind = view.potionKind;
        player.runes = view.runes;
        player.bossKeys = view.bossKeys;
        player.inTower = view.inTower;
        player.towerPrompt = view.towerPrompt;
        player.keysShown = hud.relicsShown();
        if (const auto& turbo = view.turbo) {
            player.turbo =
                HudTurbo{turbo->fill, turbo->front, turbo->back, turbo->glow, turbo->gleam};
        }
        if (view.powerup) {
            player.usage = capturePowerup(*view.powerup);
        }
        const auto& inventory = runtime.actor.save().progress().inventory;
        const auto chosen = selector.selection();
        if (selector.showing() && chosen >= 0 &&
            static_cast<usize>(chosen) < inventory.powerups.size()) {
            player.selection = capturePowerup(inventory.powerups[static_cast<usize>(chosen)]);
            player.labelY = selector.labelY(StatusBoxPainter::kY);
        }
        result.players[static_cast<usize>(id)] = std::move(player);
    }
    for (const auto& card : hud.pickups().cards()) {
        const auto found = std::ranges::find(kCards, card.texture);
        if (found == kCards.end() || card.player < 0) {
            return std::nullopt;
        }
        result.cards.push_back({static_cast<u32>(card.player),
                                static_cast<HudCardKind>(found - kCards.begin()), card.y});
    }
    for (usize seat = 0; seat < result.counts.size(); ++seat) {
        const auto& count = hud.pickups().count(static_cast<s32>(seat));
        if (!count.showing()) {
            continue;
        }
        const auto found = std::ranges::find(kCounts, count.icon);
        if (found == kCounts.end()) {
            return std::nullopt;
        }
        result.counts[seat] =
            HudCount{static_cast<HudCountKind>(found - kCounts.begin()), count.count, count.total};
    }
    if (bosses != nullptr) {
        for (usize i = 0; i < bosses->count(); ++i) {
            const auto look = bosses->meter(i).look();
            result.bossBars.push_back({look.visible, look.frozen, look.widths});
        }
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
bool ReplicaHud::load(RenderDevice& device, const std::filesystem::path& root,
                      const StringTable* strings, const HudResources& resources) {
    clear();
    if (!m_boxes.load(device, root, strings) || !m_font.load(root / "fonts/font32.json", 16)) {
        clear();
        return false;
    }
    try {
        m_messages.load(device, m_static, root, strings);
        const auto font = m_static.find("FONT32");
        const auto glow = m_static.find("FONT32_GLOW");
        const auto scroll = m_static.find("SCROLL_A");
        if (!font || !glow || !scroll || !m_messages.preloadReplica(device, strings)) {
            clear();
            return false;
        }
        m_helpScroll = &m_static.texture(device, *scroll);
        if (!m_transition.load(device, root)) {
            clear();
            return false;
        }
        m_levelTitle = resources.levelTitle;
        if (!m_helpText.load(root / "text/english.json")) {
            clear();
            return false;
        }
        if (strings != nullptr) {
            m_helpText.translate(*strings, "help");
        }
        m_help.setTexts(&m_helpText);
        m_gameOverCaption = GameOver::captionFor(m_helpText, strings);
        if (resources.powerups != nullptr && !m_hourglass.bind(device, *resources.powerups)) {
            clear();
            return false;
        }
        if (const auto frame = m_static.find("THERMBASE")) {
            m_runeFrame = &m_static.texture(device, *frame);
        }
        if (const auto column = m_static.find("THERMCOL")) {
            m_runeColumn = &m_static.texture(device, *column);
        }
        m_boxes.preloadStatus();
        m_boxes.setCountTextures(resources.counts);
        m_boxes.preloadPickups(kCards, kCounts);
        for (usize i = 0; i < kCounts.size(); ++i) {
            m_countsAvailable[i] =
                m_static.find(kCounts[i]).has_value() ||
                (resources.counts != nullptr && resources.counts->find(kCounts[i]).has_value());
        }
        m_bosses.bind(resources.bossMeters, resources.bossTextures);
        if (m_bosses.bound() && resources.bossTextures == nullptr) {
            clear();
            return false;
        }
        for (usize i = 0; i < m_bosses.count(); ++i) {
            m_bosses.meter(i).preload(device);
        }
        m_device = &device;
        m_text.setFont(&m_font, &m_static.texture(device, *font));
        m_glow = &m_static.texture(device, *glow);
        m_strings = strings;
    } catch (const std::exception& error) {
        log::warn("Replica HUD: {}", error.what());
        clear();
        return false;
    }
    return ready();
}
void ReplicaHud::clear() {
    m_messages.clear();
    m_help.clear();
    m_help.setTexts(nullptr);
    m_helpScroll = nullptr;
    m_hourglass.clear();
    m_runeFrame = nullptr;
    m_runeColumn = nullptr;
    m_transition.release();
    m_levelTitle.clear();
    m_gameOverCaption.clear();
    m_bosses.clear();
    m_device = nullptr;
    m_countsAvailable.fill(false);
    m_boxes.release();
    m_text.setFont(nullptr, nullptr);
    m_glow = nullptr;
    m_strings = nullptr;
    m_static.releaseTextures();
}
bool ReplicaHud::accepts(const HudSnapshot& snapshot) const {
    return ready() && snapshot.valid() && snapshot.bossBars.size() == m_bosses.count() &&
           (!snapshot.scroll || m_messages.accepts(scrollLook(*snapshot.scroll))) &&
           (!snapshot.help ||
            (m_helpScroll != nullptr && !m_help.linesFor(helpLook(*snapshot.help)).empty())) &&
           (!snapshot.hourglass ||
            m_hourglass.accepts({snapshot.hourglass->elapsed, snapshot.hourglass->fallingFrame})) &&
           (!snapshot.runeFill || (m_runeFrame != nullptr && m_runeColumn != nullptr)) &&
           (snapshot.screen.titleScale == 0 || !m_levelTitle.empty()) &&
           (snapshot.screen.gameOverLetters == 0 || !m_gameOverCaption.empty()) &&
           std::ranges::all_of(snapshot.counts, [&](const auto& count) {
               return !count || m_countsAvailable[static_cast<usize>(count->kind)];
           });
}
std::string_view ReplicaHud::cardTexture(HudCardKind kind) {
    return kind < HudCardKind::Count ? kCards[static_cast<usize>(kind)] : std::string_view{};
}
std::string_view ReplicaHud::countTexture(HudCountKind kind) {
    return kind < HudCountKind::Count ? kCounts[static_cast<usize>(kind)] : std::string_view{};
}
StatusBoxView ReplicaHud::status(const std::optional<HudPlayer>& player) {
    StatusBoxView view;
    if (!player) {
        return view;
    }
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = player->character;
    view.color = player->color;
    view.name = player->name;
    view.level = player->level;
    view.gold = player->gold;
    view.health = player->health;
    view.keys = player->keys;
    view.potions = player->potions;
    view.potionKind = player->potionKind;
    view.runes = static_cast<u16>(player->runes);
    view.bossKeys = static_cast<u16>(player->bossKeys);
    view.inTower = player->inTower;
    view.towerPrompt = player->towerPrompt;
    view.keysShown = player->keysShown;
    if (const auto& turbo = player->turbo) {
        view.turbo = TurboMeterLook{turbo->fill, turbo->front, turbo->back,
                                    static_cast<u8>(turbo->glow), turbo->gleam};
    }
    if (player->usage) {
        view.powerup = displayedPowerup(*player->usage);
    }
    return view;
}
void ReplicaHud::draw(Canvas& canvas, const HudSnapshot& snapshot, const Mat4& worldToCanvas) {
    if (!accepts(snapshot)) {
        return;
    }
    constexpr f32 kWidth = 512;
    constexpr f32 kHeight = 384;
    if (snapshot.screen.gameOver) {
        GameOver::drawCaption(canvas, m_text, kWidth, m_gameOverCaption,
                              snapshot.screen.gameOverLetters);
        return;
    }
    m_transition.drawOpacity(canvas, kWidth, kHeight, snapshot.screen.transitionOpacity);
    if (snapshot.visible) {
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            m_boxes.draw(canvas, static_cast<s32>(seat), status(snapshot.players[seat]), true);
        }
        for (const auto& card : snapshot.cards) {
            m_boxes.drawCard(canvas, static_cast<s32>(card.seat), cardTexture(card.kind), card.y);
        }
        for (usize seat = 0; seat < snapshot.counts.size(); ++seat) {
            if (const auto& count = snapshot.counts[seat]) {
                m_boxes.drawCount(canvas, static_cast<s32>(seat), countTexture(count->kind),
                                  count->count, count->total);
            }
        }
        if (snapshot.runeFill) {
            RuneMeter::drawFill(canvas, *m_runeFrame, *m_runeColumn, *snapshot.runeFill);
        }
        if (snapshot.hourglass) {
            m_hourglass.draw(canvas,
                             {snapshot.hourglass->elapsed, snapshot.hourglass->fallingFrame});
        }
        for (usize i = 0; i < snapshot.bossBars.size(); ++i) {
            const auto& bar = snapshot.bossBars[i];
            m_bosses.meter(i).draw(canvas, *m_device, {bar.visible, bar.frozen, bar.widths});
        }
    }
    LevelArrivalPresentation::drawTitleAt(canvas, m_text, m_levelTitle, kWidth,
                                          snapshot.screen.titleScale);
    if (snapshot.screen.cinematicBars) {
        drawCinematicBars(canvas, kHeight);
    }
    if (snapshot.visible) {
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            const auto& player = snapshot.players[seat];
            if (player && player->selection) {
                PartyHud::drawSelector(canvas, m_text, m_strings, m_glow, static_cast<s32>(seat),
                                       displayedPowerup(*player->selection), player->labelY);
            }
        }
        if (const auto& help = snapshot.help) {
            const Vec2 head = help->anchor
                                  ? PartyNames::screenOf(worldToCanvas, *help->anchor, Mat4{1})
                                        .value_or(Vec2{256, 192})
                                  : Vec2{256, 192};
            HelpMessages::drawLines(canvas, m_boxes.smallCaps(), m_helpScroll, head,
                                    m_help.linesFor(helpLook(*help)), help->player);
        }
    }
    if (snapshot.scroll) {
        m_messages.drawReplica(canvas, scrollLook(*snapshot.scroll));
    }
}
} // namespace gdl::game
