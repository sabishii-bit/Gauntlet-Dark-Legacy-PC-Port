#include "game/screens/SumnerVisit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {
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
    std::int32_t code;
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
    for (std::size_t i = 0; i < art.textures.garamond.size(); ++i) {
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
        const std::uint32_t firstRing = *ring;
        const std::uint32_t firstMask = *mask;
        try {
            art.scroll = &textures.image(*scroll);
            const auto frames = static_cast<std::uint32_t>(BurnDialogueScroll::kFrameCount);
            for (std::uint32_t i = 1; i <= frames && firstRing + i < textures.size(); ++i) {
                art.burnRing.push_back(&textures.texture(device, firstRing + i));
            }
            for (std::uint32_t i = 1; i <= frames && firstMask + i < textures.size(); ++i) {
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
    if (!m_hints.load(file)) {
        return false;
    }
    if (strings != nullptr) {
        m_hints.translate(*strings);
    }
    return true;
}

void SumnerVisit::clear() {
    m_menu.close();
    m_menu.setArt(HintMenuArt{});
    m_arrow = ModelSprite{};
    m_owner = -1;
    m_greetingLeft = -1.0f;
    m_hintsGiven = false;
}

bool SumnerVisit::visit(float seconds, std::optional<std::int32_t> visitor, bool sumnerReady,
                        const TextPainter& text, const GameConfig* config,
                        const StringTable* strings) {
    if (m_greetingLeft > 0.0f) {
        m_greetingLeft = std::max(m_greetingLeft - seconds, 0.0f);
    }
    if (!visitor.has_value()) {
        m_hintsGiven = false;
        return false;
    }
    if (m_hintsGiven || !m_hints.loaded() || !sumnerReady) {
        return false;
    }
    if (m_greetingLeft < 0.0f) {
        m_greetingLeft = kGreetingSeconds;
        return true;
    }
    if (m_greetingLeft == 0.0f) {
        m_greetingLeft = -1.0f;
        m_hintsGiven = true;
        open(*visitor, text, config, strings);
    }
    return false;
}

void SumnerVisit::open(std::int32_t player, const TextPainter& text, const GameConfig* config,
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
    labels.back = std::string(strings.get("menu.back"));
    labels.select = std::string(strings.get("menu.select"));
    labels.player = std::string(strings.get("menu.player"));
    if (const auto slot = labels.player.find("{}"); slot != std::string::npos) {
        labels.player.replace(slot, 2, std::to_string(player + 1));
    }
    MenuScreen screen;
    screen.width = static_cast<std::int32_t>(config->display.virtualWidth);
    screen.height = static_cast<std::int32_t>(config->display.virtualHeight);
    screen.horizontalFov = config->horizontalFovRadians();
    m_hints.beginVisit();
    if (m_menu.open(text, screen, std::move(labels))) {
        m_owner = player;
    }
}

HintMenuEvent SumnerVisit::update(RenderDevice& device, const MenuInput& input,
                                  std::int32_t ticks) {
    const HintMenuEvent event = m_menu.update(device, input, ticks);
    if (!m_menu.active()) {
        m_owner = -1;
    }
    return event;
}

void SumnerVisit::answer(std::int32_t topic, const TextPainter& text, const StringTable* strings,
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

} // namespace gdl::game
