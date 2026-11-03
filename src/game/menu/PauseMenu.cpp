#include "game/menu/PauseMenu.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/level/LevelWorld.h"
#include "game/widgets/MenuFont.h"

namespace gdl::game {
namespace {
constexpr s32 kQuitLevelCode = 4;
constexpr s32 kQuitDesktopCode = 8;
constexpr s32 kReturnMainMenuCode = 9;
constexpr s32 kQuitGameCode = 10;
constexpr s32 kConfirmYes = 1;
constexpr std::string_view kSealTexture = "LOGO_BURN1";
constexpr Rect kSealArea{290, 142, 224, 172};
} // namespace
PauseMenu::~PauseMenu() {
    stopSounds();
}

std::string PauseMenu::text(std::string_view id) const {
    return std::string(m_services->strings != nullptr ? m_services->strings->get(id) : id);
}
bool PauseMenu::open(RenderDevice& device, const GameServices& services,
                     std::span<const PartyMember> party, s32 player, LiveSettings* settings,
                     const LevelWorld* level) {
    close();
    m_services = &services;
    m_liveSettings = settings;
    m_device = &device;
    m_party.assign(party.begin(), party.end());
    m_player = player;
    m_inTower = level == nullptr || level->isTower();
    m_inSecretWorld = level != nullptr && level->ref().isSecret();
    if (std::ranges::none_of(party,
                             [player](const auto& member) { return member.player == player; })) {
        return false;
    }
    const auto config = services.config != nullptr ? *services.config : GameConfig{};
    m_screen = {static_cast<s32>(config.display.virtualWidth),
                static_cast<s32>(config.display.virtualHeight), config.horizontalFovRadians()};
    try {
        if (!m_textures.load(services.unpackedRoot / "STATIC") ||
            !loadMenuFont(m_font, services.unpackedRoot, 16)) {
            return false;
        }
        const auto font = m_textures.find("FONT32");
        if (!font) {
            return false;
        }
        m_art.font = &m_textures.texture(device, *font);
        for (usize i = 0; i < AudioSlider::kTextures.size(); ++i) {
            if (const auto image = m_textures.find(AudioSlider::kTextures[i])) {
                m_art.audioSlider.textures[i] = &m_textures.texture(device, *image);
            }
        }
        m_text.setFont(&m_font, m_art.font);
        if (const auto scroll = m_textures.find("SCROLL_A")) {
            m_art.backdrop = &m_textures.texture(device, *scroll);
        }
        if (const auto glow = m_textures.find("FONT32_GLOW")) {
            m_art.glow = &m_textures.texture(device, *glow);
        }
        if (const auto parchment = m_textures.find("FONT32_PARCH")) {
            m_art.parchment = &m_textures.texture(device, *parchment);
        }
        loadDecorations(device);
        loadFireFrames(device);
        if (m_services->sounds != nullptr) {
            m_commonSounds.load(services.unpackedRoot / "audio/COMMON");
        }
        showMain();
        m_open = true;
        playSound("S_OPTMENUSEL");
        return true;
    } catch (const std::exception& e) {
        log::warn("Pause menu: {}", e.what());
        close();
        return false;
    }
}
void PauseMenu::close() {
    stopSounds();
    m_settings.close();
    m_open = false;
    m_tickRemainder = 0.0;
    m_pendingOutcome = PauseOutcome::Running;
    m_fire.reset();
    m_fireMasks.clear();
    m_fireRing.clear();
    m_scrollImage = nullptr;
    m_device = nullptr;
    m_menu = OptionMenu{};
    m_settings = SettingsMenu{};
    m_art = MenuTextures{};
    m_text.setFont(nullptr, nullptr);
    m_textures.releaseTextures();
    m_powerupTextures.releaseTextures();
    m_arrow = ModelSprite{};
    m_party.clear();
}
void PauseMenu::loadDecorations(RenderDevice& device) {
    if (const auto arrows = m_textures.find("ARROWS")) {
        m_art.arrows = &m_textures.texture(device, *arrows);
    }
    if (const auto seal = m_textures.find(kSealTexture);
        seal && m_textures.size() - *seal >= m_art.burn.size()) {
        for (usize frame = 0; frame < m_art.burn.size(); ++frame) {
            m_art.burn[frame] = &m_textures.texture(device, *seal + static_cast<u32>(frame));
        }
    }
    const auto directory = m_services->unpackedRoot / "POWERUPS";
    if (m_powerupTextures.load(directory) && m_powerupModels.load(directory) &&
        m_powerupTrees.load(directory)) {
        if (const auto tree = m_powerupTrees.find("ICON_ARROW");
            tree &&
            m_arrow.bind(m_powerupTrees.tree(*tree), m_powerupModels, m_powerupTextures, device)) {
            m_art.icon = &m_arrow;
        }
    }
}
void PauseMenu::playSound(std::string_view name) {
    if (m_services->sounds == nullptr || !m_commonSounds.loaded()) {
        return;
    }
    const auto index = m_commonSounds.find(name);
    if (!index) {
        return;
    }
    try {
        std::erase_if(m_soundHandles,
                      [&](SoundHandle handle) { return !m_services->sounds->isPlaying(handle); });
        const auto handle =
            m_services->sounds->play(m_commonSounds.sequence(*index), 1, SoundCategory::Effects);
        if (handle != kNoSound) {
            m_soundHandles.push_back(handle);
        }
    } catch (const std::exception& error) {
        log::warn("Pause menu sound {}: {}", name, error.what());
    }
}

void PauseMenu::stopSounds() {
    // Queued sequences borrow COMMON's samples. Stop only this menu's voices before reloading
    // or destroying its bank.
    if (m_services->sounds != nullptr) {
        for (const auto handle : m_soundHandles) {
            m_services->sounds->stop(handle);
        }
    }
    m_soundHandles.clear();
}

void PauseMenu::loadFireFrames(RenderDevice& device) {
    const auto ring = m_textures.find("GREENCIRCTRANS");
    const auto mask = m_textures.find("GREENCIRCTRANSM");
    const auto scroll = m_textures.find("SCROLL_A");
    constexpr auto kFrames = static_cast<u32>(BurnDialogueScroll::kFrameCount);
    if (!ring || !mask || !scroll || m_textures.size() - *ring <= kFrames ||
        m_textures.size() - *mask <= kFrames) {
        log::warn("Pause menu: scroll dismissal textures are missing");
        return;
    }
    for (u32 frame = 1; frame <= kFrames; ++frame) {
        m_fireRing.push_back(&m_textures.texture(device, *ring + frame));
        m_fireMasks.push_back(&m_textures.image(*mask + frame));
    }
    m_scrollImage = &m_textures.image(*scroll);
}

PauseOutcome PauseMenu::dismiss(PauseOutcome outcome) {
    if (m_device == nullptr || m_scrollImage == nullptr ||
        !m_fire.start(*m_device, m_menu.backdropArea(), *m_scrollImage, m_fireMasks, m_fireRing)) {
        return outcome;
    }
    m_pendingOutcome = outcome;
    m_menu.releaseBackdrop();
    m_menu.closeWithFade();
    playSound("S_OPTMENUSCROLL");
    return PauseOutcome::Running;
}

void PauseMenu::playMenuSound(const MenuEvent& event, bool horizontal) {
    if (event.action == MenuAction::Moved) {
        playSound(horizontal ? "S_OPTMENUMOVHRZ" : "S_OPTMENUMOVVRT");
    } else if (event.action == MenuAction::Choice) {
        playSound("S_OPTMENUSEL");
    } else if (event.action == MenuAction::Back) {
        playSound("S_OPTMENUEXIT");
    }
}
MenuDefinition PauseMenu::backdrop() const {
    auto menu = MenuDefinition::parchment();
    menu.backdropX = 16;
    menu.burn = kSealTexture;
    menu.burnArea = kSealArea;
    menu.playerLabel = std::format("{}: {}", text("files.player"), m_player + 1);
    return menu;
}
void PauseMenu::showMain() {
    m_page = Page::Main;
    auto menu = backdrop();
    menu.title = text(m_inTower ? "pause.tower" : "pause.game");
    menu.items = {{text("pause.settings"), 3}};
    // The tower's menu manages the party and, through Quit Game, leaves for the main menu
    // or the desktop; a level's offers Quit Level, back to the tower, as the original does.
    if (m_inTower) {
        menu.items.push_back({text("pause.manage"), 5});
        menu.items.push_back({text("pause.shop"), 6});
        menu.items.push_back({text("pause.inventory"), 7});
        menu.items.push_back({text("pause.quit"), kQuitGameCode});
    } else {
        // The secret world cannot be quit (options.c 1462: OPT_QUITLEVEL greyed for world 12).
        menu.items.push_back({text("pause.quitLevel"), kQuitLevelCode, 0, !m_inSecretWorld});
    }
    m_menu.open(menu, m_text, m_screen);
}
/** The retail yes/no dialog (0x8011E8E0 "Abort Level?"): a small centred parchment fading
 * in, with only the title and No then Yes, No first. */
void PauseMenu::showQuitLevel() {
    m_page = Page::QuitLevel;
    m_menu.open(MenuDefinition::confirmation(text("pause.abortConfirm"), text("pause.no"),
                                             text("pause.yes")),
                m_text, m_screen);
}
/** Quit Game's choice, on the dialog's parchment with no title: back to the main menu or out
 * to the desktop, each taken at once. */
void PauseMenu::showQuitGame() {
    m_page = Page::QuitGame;
    auto menu =
        MenuDefinition::confirmation({}, text("pause.returnMainMenu"), text("menu.quitDesktop"));
    menu.items[0].code = kReturnMainMenuCode;
    menu.items[1].code = kQuitDesktopCode;
    m_menu.open(menu, m_text, m_screen);
}
PauseOutcome PauseMenu::update(f64 seconds, const MenuInput& rawInput) {
    if (!m_open) {
        return PauseOutcome::Running;
    }
    const auto input = mapMenuPointer(rawInput, m_pointerTransform);
    const auto rate = m_services->config != nullptr ? m_services->config->timing.tickRate
                                                    : TimingConfig{}.tickRate;
    m_tickRemainder += std::max(seconds, 0.0) * rate;
    constexpr f64 kRoundingTolerance = 1.0e-9;
    const auto ticks = static_cast<s32>(std::floor(m_tickRemainder + kRoundingTolerance));
    m_tickRemainder = std::max(0.0, m_tickRemainder - ticks);
    if (m_pendingOutcome != PauseOutcome::Running) {
        m_menu.update({}, ticks);
        m_fire.step(ticks);
        return m_fire.active() ? PauseOutcome::Running : m_pendingOutcome;
    }
    if (m_page == Page::Options) {
        const auto depth = m_settings.depth();
        const auto event = m_settings.update(input, ticks);
        playMenuSound(event, input.left || input.right);
        if (!m_settings.audioSample().empty()) {
            playSound(m_settings.audioSample());
        }
        // A step back within the settings, by Back or by choosing a Back row.
        if (event.action == MenuAction::None && m_settings.depth() < depth) {
            playSound("S_OPTMENUEXIT");
        }
        if (event.action == MenuAction::Back) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    auto mapped = input;
    mapped.back |= input.escape;
    const auto event = m_menu.update(mapped, ticks);
    if (m_page == Page::Main &&
        (event.action == MenuAction::Back || (input.start && !input.select))) {
        // Resuming is not a submenu confirmation/back step: only the scroll
        // dismissal should sound, without the extra navigation ding.
        return dismiss(PauseOutcome::Resume);
    }
    playMenuSound(event, false);
    if (m_page == Page::QuitLevel) {
        if (event.action == MenuAction::Choice && event.code == kConfirmYes) {
            return dismiss(PauseOutcome::ReturnTower);
        }
        if (event.action == MenuAction::Back || event.action == MenuAction::Choice) {
            showMain(); // No, or a step back
        }
        return PauseOutcome::Running;
    }
    if (m_page == Page::QuitGame) {
        if (event.action == MenuAction::Choice && event.code == kReturnMainMenuCode) {
            return dismiss(PauseOutcome::Title);
        }
        if (event.action == MenuAction::Choice && event.code == kQuitDesktopCode) {
            return dismiss(PauseOutcome::Desktop);
        }
        if (event.action == MenuAction::Back) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    if (event.action != MenuAction::Choice) {
        return PauseOutcome::Running;
    }
    if (event.code == 3) {
        m_page = Page::Options;
        m_settings.setMechanics(m_services->mechanics);
        m_settings.open(m_services->config != nullptr ? *m_services->config : GameConfig{},
                        m_services->strings, m_liveSettings, m_text, m_screen, backdrop(),
                        m_inTower ? SettingsMenu::Scope::Tower : SettingsMenu::Scope::Level);
    } else if (event.code == 5) {
        return PauseOutcome::Manage;
    } else if (event.code == 6) {
        return PauseOutcome::Shop;
    } else if (event.code == 7) {
        return PauseOutcome::Inventory;
    } else if (event.code == kQuitLevelCode) {
        showQuitLevel();
    } else if (event.code == kQuitGameCode) {
        showQuitGame();
    }
    return PauseOutcome::Running;
}
bool PauseMenu::musicAudible() const {
    return m_open && m_page == Page::Options && m_settings.page() == SettingsMenu::Page::Audio;
}
void PauseMenu::prepare(RenderDevice& device) {
    if (m_open) {
        m_fire.prepare(device);
    }
}

void PauseMenu::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (!m_open) {
        return;
    }
    const auto virtualWidth = static_cast<f32>(m_screen.width);
    const auto virtualHeight = static_cast<f32>(m_screen.height);
    m_pointerTransform =
        makeVirtualScreenTransform(projection, virtualWidth, virtualHeight, width, height);
    m_canvas.begin(device, m_pointerTransform);
    m_canvas.fillScreen(Color::rgba(0, 0, 0, 150));
    m_fire.draw(m_canvas);
    auto art = m_art;
    if (m_page == Page::QuitLevel || m_page == Page::QuitGame ||
        (m_page == Page::Options && m_settings.page() != SettingsMenu::Page::Root &&
         m_settings.page() != SettingsMenu::Page::Compass)) {
        art.burn.fill(nullptr);
    }
    if (m_page == Page::Options) {
        m_settings.draw(m_canvas, m_text, art);
    } else {
        m_menu.draw(m_canvas, m_text, art);
    }
    m_canvas.end();
}
} // namespace gdl::game
