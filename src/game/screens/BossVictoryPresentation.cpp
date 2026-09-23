#include "game/screens/BossVictoryPresentation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/core/Log.h"

#include "game/menu/ScrollBox.h"

namespace gdl::game {
namespace {
constexpr std::string_view kWizardTree = "WIZARD";
constexpr float kWizardLift = 3.0f;
constexpr float kCaptionScale = 0.75f;
constexpr int kCaptionBottom = 96;
constexpr int kCaptionLineHeight = 18;
} // namespace

void BossVictoryPresentation::begin(int kind, char realm, std::uint16_t runesInRealm,
                                    std::uint16_t runesFound, bool goldLeft) {
    clear();
    m_visit.begin(kind, realm, runesInRealm, runesFound, goldLeft);
}

void BossVictoryPresentation::clear() {
    m_visit.clear();
    m_model.clear();
    m_player.stop();
    m_tree = nullptr;
    m_pose = TreePose{};
    m_position = Vec3{0.0f};
    m_yaw = 0.0f;
    m_sparkled = false;
}

void BossVictoryPresentation::bindWizard(RenderDevice& device, ItemArchive& items, const Vec3& boss,
                                         std::span<const Vec3> party) {
    m_tree = nullptr;
    m_model.clear();
    m_player.stop();
    m_pose = TreePose{};
    const auto tree = items.trees.find(kWizardTree);
    if (!tree.has_value()) {
        log::warn("Tower: no {} in the level's items; the wizard is not seen", kWizardTree);
        return;
    }
    const TreeInfo& figure = items.trees.tree(*tree);
    if (!m_model.bind(figure, items.models, items.textures, device)) {
        return;
    }
    m_tree = &figure;
    if (!figure.sequences.empty()) {
        m_player.start(figure.sequences[0], 0);
    }
    m_pose.rest(figure);
    // Over the middle of the boss's mark and the party, facing the party.
    Vec3 centre = boss;
    float count = 1.0f;
    Vec3 partyCentre{0.0f};
    for (const Vec3& position : party) {
        centre += position;
        partyCentre += position;
        count += 1.0f;
    }
    m_position = centre / count + Vec3{0.0f, kWizardLift, 0.0f};
    const Vec3 toParty = partyCentre / std::max(count - 1.0f, 1.0f) - m_position;
    m_yaw = std::atan2(toParty.x, toParty.z);
}

BossVictoryPresentation::Update BossVictoryPresentation::update(int ticks, float seconds,
                                                                bool goldLeft,
                                                                const MessageTable& strings) {
    Update result;
    if (!m_visit.running()) {
        return result;
    }
    m_visit.setGoldLeft(goldLeft);
    std::vector<std::size_t> pageLengths;
    if (const auto& caption = m_visit.caption(); caption.has_value()) {
        if (const auto found = strings.find(caption->message); found.has_value()) {
            for (const std::string& page : strings.message(*found).pages) {
                pageLengths.push_back(page.size());
            }
        }
    }
    result.voices = m_visit.update(ticks, pageLengths);
    if (m_tree != nullptr && m_player.playing() && m_visit.wizardShown()) {
        m_player.advance(seconds, true);
        m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    }
    if (m_visit.sparkling() && !m_sparkled) {
        m_sparkled = true;
        result.sparkle = true;
    }
    return result;
}

BossCameraSubject BossVictoryPresentation::wizardSubject() const {
    BossCameraSubject subject;
    subject.position = m_position;
    subject.facing = m_yaw;
    subject.height = kWizardLift;
    subject.awake = true;
    return subject;
}

void BossVictoryPresentation::drawWizard(RenderDevice& device, const Mat4& clip,
                                         const WorldLighting& lighting) const {
    if (m_tree == nullptr || !m_visit.wizardShown() || m_visit.wizardAlpha() <= 0.0f) {
        return;
    }
    const Mat4 model =
        glm::rotate(glm::translate(Mat4{1.0f}, m_position), m_yaw, Vec3{0.0f, 1.0f, 0.0f});
    m_model.draw(device, clip, model, lighting, m_pose.matrices(), nullptr, m_visit.wizardAlpha());
}

void BossVictoryPresentation::drawCaption(Canvas& canvas, const TextPainter& text,
                                          const MessageTable& strings, float width,
                                          float height) const {
    const auto& caption = m_visit.caption();
    if (!caption.has_value() || !text.ready()) {
        return;
    }
    const auto found = strings.find(caption->message);
    if (!found.has_value() || caption->page >= strings.message(*found).pages.size()) {
        return;
    }
    const std::string shown =
        strings.message(*found).pages[caption->page].substr(0, caption->shown);
    const std::vector<std::string> lines = ScrollBox::splitLines(shown);
    TextStyle style;
    style.scale = kCaptionScale;
    int y = static_cast<int>(height) - kCaptionBottom -
            static_cast<int>(lines.size()) * kCaptionLineHeight;
    for (const std::string& line : lines) {
        text.draw(canvas, -static_cast<int>(width / 2.0f), y, line, style);
        y += kCaptionLineHeight;
    }
}

} // namespace gdl::game
