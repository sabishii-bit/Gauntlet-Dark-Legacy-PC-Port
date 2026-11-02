#include "game/tower/SumnerVisit.h"

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

namespace gdl::game {
bool SumnerVisit::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("SumnerVisit.native.1");
    digest.fields<3>(m_sim);
    return m_menu.appendNativeDigest(digest) && m_hints.appendNativeDigest(digest);
}

bool SumnerVisit::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, binding, menu, hints] = saved;
    digest.string("SumnerVisit.native.1");
    digest.fields<3>(state);
    return m_menu.appendLocalDigest(digest, menu) && m_hints.appendLocalDigest(digest, hints);
}

namespace {
constexpr std::string_view kHintTextFile = "text/hints_e.json";
constexpr std::string_view kArrowTree = "ICON_ARROW";
constexpr std::string_view kFontTexture = "FONT32";
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kFireRingTexture = "GREENCIRCTRANS";
constexpr std::string_view kFireMaskTexture = "GREENCIRCTRANSM";
/** Sumner's topics: the text id of each and the code the original's menu gives it. */
struct HintTopicEntry {
    std::string_view text;
    s32 code;
    HintTopic topic;
};
constexpr std::array<HintTopicEntry, 4> kHintTopics{
    {{"hints.general", 39, HintTopic::General},
     {"hints.guardians", 40, HintTopic::Guardians},
     {"hints.legends", 41, HintTopic::Legends},
     {"hints.runestones", 42, HintTopic::Runestones}}};

} // namespace

const Texture* SumnerVisit::load(RenderDevice& device, TextureSet& textures, ItemArchive& powerups,
                                 const std::filesystem::path& root, const StringTable* strings) {
    HintMenuArt art;
    if (!textures.loaded() || !loadTexts(root / kHintTextFile, strings)) {
        log::warn("Tower: Sumner's hints are not unpacked; he has nothing to say");
        m_menu.setArt(std::move(art));
        return nullptr;
    }
    const auto texture = [&](std::string_view name) -> const Texture* {
        const auto index = textures.find(name);
        try {
            return index.has_value() ? &textures.texture(device, *index) : nullptr;
        } catch (const std::exception& e) {
            log::warn("Tower: texture {}: {}", name, e.what());
            return nullptr;
        }
    };
    art.textures.font = texture(kFontTexture);
    art.textures.glow = texture("FONT32_GLOW");
    const Texture* glow = art.textures.glow;
    art.textures.parchment = texture("FONT32_PARCH");
    art.textures.arrows = texture("ARROWS");
    for (usize i = 0; i < art.textures.garamond.size(); ++i) {
        art.textures.garamond[i] = texture(std::format("FONT32GAR{}", i));
    }
    art.textures.backdrop = texture(kScrollTexture);
    if (const auto tree = powerups.trees.find(kArrowTree);
        powerups.loaded() && tree.has_value() &&
        m_arrow.bind(powerups.trees.tree(*tree), powerups.models, powerups.textures, device)) {
        art.textures.icon = &m_arrow;
    }
    const auto scroll = textures.find(kScrollTexture);
    const auto ring = textures.find(kFireRingTexture);
    const auto mask = textures.find(kFireMaskTexture);
    if (scroll.has_value() && ring.has_value() && mask.has_value()) {
        const u32 firstRing = *ring;
        const u32 firstMask = *mask;
        try {
            art.scroll = &textures.image(*scroll);
            const auto frames = static_cast<u32>(BurnDialogueScroll::kFrameCount);
            for (u32 i = 1; i <= frames && firstRing + i < textures.size(); ++i) {
                art.burnRing.push_back(&textures.texture(device, firstRing + i));
            }
            for (u32 i = 1; i <= frames && firstMask + i < textures.size(); ++i) {
                art.burnMasks.push_back(&textures.image(firstMask + i));
            }
        } catch (const std::exception& e) {
            log::warn("Tower: burn frames: {}", e.what());
            art.scroll = nullptr;
        }
    }
    m_menu.setArt(std::move(art));
    return glow;
}

bool SumnerVisit::loadTexts(const std::filesystem::path& file, const StringTable* strings) {
    m_localBinding = std::make_shared<const u8>(0);
    if (!m_hints.load(file)) {
        return false;
    }
    if (strings != nullptr) {
        m_hints.translate(*strings);
    }
    return true;
}

bool SumnerVisit::openTopics(s32 player, const TextPainter& text, const MenuScreen& screen,
                              HintMenuLabels labels) {
    if (!m_menu.open(text, screen, std::move(labels))) {
        return false;
    }
    m_sim.owner = player;
    return true;
}

void SumnerVisit::closeTopics() {
    m_menu.close();
    m_sim.owner = -1;
}

void SumnerVisit::release(s32 player) {
    if (player >= 0 && player == m_sim.owner) {
        closeTopics();
    }
}

void SumnerVisit::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_menu.close();
    m_menu.setArt(HintMenuArt{});
    m_arrow = ModelSprite{};
    m_sim.owner = -1;
    m_sim.greetingLeft = -1.0f;
    m_sim.hintsGiven = false;
}

bool SumnerVisit::visit(f32 seconds, std::optional<s32> visitor, bool sumnerReady,
                        const TextPainter& text, const GameConfig* config,
                        const StringTable* strings) {
    if (m_sim.greetingLeft > 0.0f) {
        m_sim.greetingLeft = std::max(m_sim.greetingLeft - seconds, 0.0f);
    }
    if (!visitor.has_value()) {
        m_sim.hintsGiven = false;
        return false;
    }
    if (m_sim.hintsGiven || !m_hints.loaded() || !sumnerReady) {
        return false;
    }
    if (m_sim.greetingLeft < 0.0f) {
        m_sim.greetingLeft = kGreetingSeconds;
        return true;
    }
    if (m_sim.greetingLeft == 0.0f) {
        m_sim.greetingLeft = -1.0f;
        m_sim.hintsGiven = true;
        open(*visitor, text, config, strings);
    }
    return false;
}

void SumnerVisit::open(s32 player, const TextPainter& text, const GameConfig* config,
                       const StringTable* stringsTable) {
    if (stringsTable == nullptr || config == nullptr) {
        return;
    }
    const StringTable& strings = *stringsTable;
    HintMenuLabels labels;
    labels.title = std::string(strings.get("hints.title"));
    for (const HintTopicEntry& topic : kHintTopics) {
        labels.topics.push_back(MenuItem{std::string(strings.get(topic.text)), topic.code});
    }
    labels.select = std::string(strings.get("menu.select"));
    labels.player = std::string(strings.get("menu.player"));
    if (const auto slot = labels.player.find("{}"); slot != std::string::npos) {
        labels.player.replace(slot, 2, std::to_string(player + 1));
    }
    MenuScreen screen;
    screen.width = static_cast<s32>(config->display.virtualWidth);
    screen.height = static_cast<s32>(config->display.virtualHeight);
    screen.horizontalFov = config->horizontalFovRadians();
    m_hints.beginVisit();
    if (m_menu.open(text, screen, std::move(labels))) {
        m_sim.owner = player;
    }
}

HintMenuEvent SumnerVisit::update(RenderDevice& device, const MenuInput& input, s32 ticks) {
    return updateNative(device, m_sim.owner, input, ticks, 0, 0);
}

u8 SumnerVisit::captureTopic(s32 player, const MenuInput& input) const {
    return player == m_sim.owner && player >= 0 ? m_menu.captureTopic(input) : 0;
}

HintMenuEvent SumnerVisit::updateNative(RenderDevice& device, s32 player, const MenuInput& input,
                                        s32 ticks, u8 topic, u8 revision) {
    const bool owned = player >= 0 && player == m_sim.owner;
    const HintMenuEvent event =
        m_menu.update(device, owned ? input : MenuInput{}, ticks, owned ? topic : 0, revision);
    if (!m_menu.active()) {
        m_sim.owner = -1;
    }
    return event;
}

void SumnerVisit::answer(s32 topic, const TextPainter& text, const StringTable* strings,
                         const HintKnowledge& knowledge) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto entry = std::ranges::find(kHintTopics, topic, &HintTopicEntry::code);
    if (entry == kHintTopics.end() || strings == nullptr) {
        return;
    }
    HintPage page = m_hints.next(entry->topic, knowledge, strings->get("hints.generalTitle"));
    m_menu.read(text, std::move(page.title), std::move(page.passages), page.scale, page.centred,
                page.gap);
}

void SumnerVisit::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    m_menu.saveLocal(saved.m_menu);
    m_hints.saveLocal(saved.m_hints);
}

bool SumnerVisit::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && saved.m_binding.lock() == m_localBinding &&
           m_menu.canRestoreLocal(saved.m_menu) && m_hints.canRestoreLocal(saved.m_hints);
}

bool SumnerVisit::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_menu.restoreLocal(saved.m_menu);
    m_hints.restoreLocal(saved.m_hints);
    return true;
}

} // namespace gdl::game
