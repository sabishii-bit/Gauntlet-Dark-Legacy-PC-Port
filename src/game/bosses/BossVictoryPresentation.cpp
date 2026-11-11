#include "game/bosses/BossVictoryPresentation.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/bosses/BossTraits.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {
void BossVictoryPresentation::appendState(NativeStateDigest& digest, const Simulation& state,
                                          const TextureAnimator::Clock& clock) const {
    const auto& [visit, tree, player, pose, previousFrame, previousGeneration, advanced, position,
                 yaw, sparkled] = state;
    digest.string("BossVictoryPresentation.native.1");
    digest.value(visit);
    digest.resource(tree);
    digest.values(player, pose, previousFrame, previousGeneration, advanced, position, yaw,
                  sparkled);
    // The first speculative boss fall may warm this retained draw bank. A
    // pre-fall snapshot has no wizard and must not hash a later cache birth.
    if (tree) {
        m_textures.appendBindingDigest(digest);
    }
    digest.fields<3>(clock);
}

bool BossVictoryPresentation::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim, m_textures.clock());
    return digest.good();
}

bool BossVictoryPresentation::appendLocalDigest(NativeStateDigest& digest,
                                                const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, clock, owner, binding] = saved;
    appendState(digest, state, clock);
    return digest.good();
}

void BossVictoryPresentation::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
    saved.m_textureClock = m_textures.clock();
}

bool BossVictoryPresentation::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding;
}

bool BossVictoryPresentation::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_textures.restoreClock(saved.m_textureClock);
    return true;
}

namespace {
constexpr std::string_view kWizardTree = "WIZARD";
constexpr f32 kWizardLift = 3.0f;
constexpr f32 kCaptionScale = 0.667f; ///< CaptionText: two thirds of the scroll's scale
constexpr f32 kCaptionTop = 16.0f;    ///< do_good_wizard lays its captions at y 16 of 384
constexpr f32 kCaptionLineHeight = 32.0f * kCaptionScale;
} // namespace

void BossVictoryPresentation::begin(s32 kind, char realm, u16 runesInRealm, u16 runesFound,
                                    bool goldLeft) {
    reset();
    m_sim.visit.begin(kind, realm, runesInRealm, runesFound, goldLeft);
}

void BossVictoryPresentation::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    reset();
    m_model.clear();
    m_textures = TextureAnimator{};
    m_boundDevice = nullptr;
    m_boundItems = nullptr;
}

void BossVictoryPresentation::reset() {
    m_sim.visit.clear();
    m_sim.player.stop();
    m_sim.tree = nullptr;
    m_sim.pose = TreePose{};
    m_sim.presentationAdvanced = false;
    m_textures.restoreClock({});
    m_sim.position = Vec3{0.0f};
    m_sim.yaw = 0.0f;
    m_sim.sparkled = false;
}

void BossVictoryPresentation::bindWizard(RenderDevice& device, ItemArchive& items, const Vec3& boss,
                                         std::span<const Vec3> party) {
    m_sim.tree = nullptr;
    m_sim.player.stop();
    m_sim.pose = TreePose{};
    m_sim.presentationAdvanced = false;
    if ((m_boundDevice != nullptr || m_boundItems != nullptr) &&
        (m_boundDevice != &device || m_boundItems != &items)) {
        m_localBinding = std::make_shared<const u8>(0);
        m_model.clear();
        m_textures = TextureAnimator{};
        m_boundDevice = nullptr;
        m_boundItems = nullptr;
    }
    const auto tree = items.trees.find(kWizardTree);
    if (!tree.has_value()) {
        log::warn("Tower: no {} in the level's items; the wizard is not seen", kWizardTree);
        return;
    }
    const TreeInfo& figure = items.trees.tree(*tree);
    if (m_boundDevice != &device || m_boundItems != &items) {
        m_model.clear();
        m_boundDevice = nullptr;
        m_boundItems = nullptr;
        if (!m_model.bind(figure, items.models, items.textures, device)) {
            return;
        }
        m_textures.bind(items.trees.textureAnimations(), items.textures, device);
        m_boundDevice = &device;
        m_boundItems = &items;
    }
    m_sim.tree = &figure;
    // DoGoodWizard creates this tree with 0x881880: additive (0x800000)
    // and no depth writes (0x80), including the animated lower-body meshes.
    m_model.setAppearance({.unlit = true, .depthWrite = false, .additive = true});
    m_textures.restoreClock({});
    if (!figure.sequences.empty()) {
        m_sim.player.start(figure.sequences[0], 0);
    }
    m_sim.pose.rest(figure);
    if (!figure.sequences.empty()) {
        m_sim.pose.evaluate(figure, 0, 0.0f);
        m_model.setFrame(0, 0);
        m_textures.apply(m_model, figure, 0, 0);
    }
    // Over the middle of the boss's mark and the party, facing the party.
    Vec3 centre = boss;
    f32 count = 1.0f;
    Vec3 partyCentre{0.0f};
    for (const Vec3& position : party) {
        centre += position;
        partyCentre += position;
        count += 1.0f;
    }
    m_sim.position = centre / count + Vec3{0.0f, kWizardLift, 0.0f};
    if (const BossTraits* traits = bossTraitsOf(m_sim.visit.kind());
        traits != nullptr && traits->defeat.wizardAtAltar) {
        // Both Skorne arenas use DoGoodWizard's fixed altar position, not the
        // ordinary boss/party midpoint (0x80345a08..0x80345a10).
        m_sim.position = Vec3{0, -12, 6};
    }
    const Vec3 toParty = partyCentre / std::max(count - 1.0f, 1.0f) - m_sim.position;
    m_sim.yaw = portable::atan2(toParty.x, toParty.z);
}

BossVictoryPresentation::Update BossVictoryPresentation::update(s32 ticks, f32 seconds,
                                                                bool goldLeft,
                                                                const MessageTable& strings,
                                                                bool voicePlaying) {
    Update result;
    m_textures.advance(0);
    m_sim.previousFrame = m_sim.player.presentationFrame();
    m_sim.previousGeneration = m_sim.player.generation();
    m_sim.presentationAdvanced = seconds > 0 && m_sim.visit.wizardShown() && m_sim.player.playing();
    if (!m_sim.visit.running()) {
        return result;
    }
    m_sim.visit.setGoldLeft(goldLeft);
    std::span<const std::string> pages;
    if (const auto& caption = m_sim.visit.caption(); caption.has_value()) {
        if (const auto found = strings.find(caption->message); found.has_value()) {
            pages = strings.message(*found).pages;
        }
    }
    result.voices = m_sim.visit.update(ticks, pages, voicePlaying);
    if (m_sim.tree != nullptr && m_sim.player.playing() && m_sim.visit.wizardShown()) {
        m_sim.player.advance(seconds, true);
        m_sim.pose.evaluate(*m_sim.tree, m_sim.player.sequence(), m_sim.player.frame());
        m_model.setFrame(m_sim.player.sequence(), static_cast<s32>(m_sim.player.frame()));
        m_textures.advance(seconds);
        m_textures.apply(m_model, *m_sim.tree, m_sim.player.sequence(),
                         static_cast<s32>(m_sim.player.frame()));
    }
    if (m_sim.visit.sparkling() && !m_sim.sparkled) {
        m_sim.sparkled = true;
        result.sparkle = true;
    }
    return result;
}

BossCameraSubject BossVictoryPresentation::wizardSubject() const {
    BossCameraSubject subject;
    subject.position = m_sim.position;
    subject.facing = m_sim.yaw;
    // DoGoodWizard's camera target is ten units above its parent transform.
    subject.attentionOffset = Vec3{0, 10, 0};
    subject.height = std::max(0.0f, m_model.maxBounds().y);
    subject.focus = BossCameraSubject::Focus::Wizard;
    subject.engaged = true;
    return subject;
}

void BossVictoryPresentation::drawWizard(RenderDevice& device, const Mat4& clip,
                                         const WorldLighting& lighting, const CameraFrame* camera,
                                         f32 frameBlend) const {
    if (m_sim.tree == nullptr || !m_sim.visit.wizardShown() || m_sim.visit.wizardAlpha() <= 0.0f) {
        return;
    }
    const Mat4 model = portable::rotate(glm::translate(Mat4{1.0f}, m_sim.position), m_sim.yaw,
                                        Vec3{0.0f, 1.0f, 0.0f});
    TreePose visualPose;
    const TreePose* pose = &m_sim.pose;
    const f32 blend = frameBlend < 0 || m_sim.presentationAdvanced ? frameBlend : 1.0f;
    f32 frame = m_sim.player.frame();
    if (blend >= 0 && m_sim.player.playing()) {
        frame = m_sim.previousGeneration == m_sim.player.generation()
                    ? portable::lerp(m_sim.previousFrame, m_sim.player.presentationFrame(),
                                     std::clamp(blend, 0.0f, 1.0f))
                    : m_sim.player.presentationFrame();
        visualPose.evaluate(*m_sim.tree, m_sim.player.sequence(), frame,
                            TreePose::Sampling::Smooth);
        pose = &visualPose;
    }
    m_model.setPresentationFrame(m_sim.player.sequence(), frame);
    m_textures.apply(m_model, *m_sim.tree, m_sim.player.sequence(), frame,
                     m_textures.presentationOffset(blend));
    m_model.draw(device, clip, model, lighting, pose->matrices(), camera,
                 m_sim.visit.wizardAlpha());
}

void BossVictoryPresentation::drawCaption(Canvas& canvas, const TextPainter& text,
                                          const MessageTable& strings, f32 width,
                                          f32 height) const {
    const auto& caption = m_sim.visit.caption();
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
    // Unlike the tower's ceremony captions over the status boxes, the wizard's visit after
    // a boss writes at the top of the screen, each line centred.
    TextStyle style;
    style.scale = kCaptionScale;
    f32 y = height * kCaptionTop / 384.0f;
    for (const std::string& line : lines) {
        const auto x = text.leftEdge(-static_cast<s32>(width / 2.0f), line, style.scale);
        text.draw(canvas, x, static_cast<s32>(y), line, style);
        y += kCaptionLineHeight;
    }
}

} // namespace gdl::game
