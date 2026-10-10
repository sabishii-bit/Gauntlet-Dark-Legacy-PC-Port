#include "game/world/PowerupCompanion.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

using Action = PlayerAnimator::Action;

/** The specials that bring a companion (0x7004F1), whose time left fades it. */
constexpr u32 kCompanionSpecials = 0x7004F1;
constexpr f32 kFadeSeconds = 1.0f;

} // namespace

PowerupCompanion::Kind PowerupCompanion::choose(const PowerupEffects& worn, bool shieldRunning) {
    if ((worn.special & powerup::kPojo) != 0) {
        return Kind::Pojo;
    }
    if ((worn.armor & powerup::kFireShield) != 0 && shieldRunning) {
        return Kind::FireShield;
    }
    if ((worn.special & powerup::kPhoenix) != 0) {
        return Kind::Phoenix;
    }
    if ((worn.special & powerup::kFireBreath) != 0) {
        return Kind::FireBreath;
    }
    if ((worn.special & powerup::kAcidBreath) != 0) {
        return Kind::AcidBreath;
    }
    if ((worn.special & powerup::kLightningBreath) != 0) {
        return Kind::ElecBreath;
    }
    return (worn.special & powerup::kLevitation) != 0 ? Kind::Wings : Kind::None;
}

std::string_view PowerupCompanion::treeOf(Kind kind) {
    switch (kind) {
    case Kind::Pojo: return "POJO";
    case Kind::FireShield: return "FW_SHLD_ACTIVE";
    case Kind::Phoenix: return "PHOENIX";
    case Kind::FireBreath: return "HEAD_BREATHEF";
    case Kind::AcidBreath: return "HEAD_BREATHEA";
    case Kind::ElecBreath: return "HEAD_BREATHEE";
    case Kind::Wings: return "WINGS";
    case Kind::None: break;
    }
    return {};
}

PowerupCompanion::Mount PowerupCompanion::mountOf(Kind kind) {
    switch (kind) {
    case Kind::FireBreath:
    case Kind::AcidBreath:
    case Kind::ElecBreath: return Mount::Head;
    case Kind::Wings: return Mount::Back;
    default: return Mount::Body;
    }
}

s32 PowerupCompanion::pojoSequenceOf(Action action) {
    switch (action) {
    case Action::Shove:
    case Action::Walk1:
    case Action::Walk2:
    case Action::Run1:
    case Action::Run2:
    case Action::ShieldRun:
    case Action::Pushed: return kRun;
    case Action::Breathe: return kPower;
    case Action::HitReact:
    case Action::Stun:
    case Action::WebReact:
    case Action::FallBack:
    case Action::FallForward:
    case Action::Whirled:
    case Action::Grabbed: return kHit;
    case Action::Death: return kDeath;
    default: return kReady;
    }
}

f32 PowerupCompanion::fadeOf(const Inventory& inventory) {
    f32 left = -1.0f; // none found yet
    for (const PowerupSlot& slot : inventory.powerups) {
        if (!slot.working()) {
            continue;
        }
        const bool brings =
            (slot.kind == powerup::kSpecial && (slot.flags & kCompanionSpecials) != 0) ||
            (slot.kind == powerup::kArmor && (slot.flags & powerup::kFireShield) != 0);
        if (brings) {
            left = std::max(left, slot.strength < 0.0f ? kFadeSeconds : slot.strength);
        }
    }
    return left < 0.0f ? 1.0f : std::clamp(left / kFadeSeconds, 0.0f, 1.0f);
}

void PowerupCompanion::choose(RenderDevice& device, Kind kind, ItemArchive& powerups,
                              ItemArchive* weapons) {
    if (kind == m_kind && (kind == Kind::None || m_tree != nullptr)) {
        return;
    }
    clear();
    m_kind = kind;
    ItemArchive* archive = fromWeapons(kind) ? weapons : &powerups;
    if (kind == Kind::None || archive == nullptr || !archive->loaded()) {
        return;
    }
    const auto index = archive->trees.find(treeOf(kind));
    if (!index.has_value()) {
        return;
    }
    const TreeInfo& tree = archive->trees.tree(*index);
    if (tree.sequences.empty() || !m_model.bind(tree, archive->models, archive->textures, device)) {
        return;
    }
    m_tree = &tree;
    m_textures.bind(archive->trees.textureAnimations(), archive->textures, device);
    play(kReady);
}

void PowerupCompanion::play(s32 sequence) {
    if (m_tree == nullptr) {
        return;
    }
    if (sequence < 0 || static_cast<usize>(sequence) >= m_tree->sequences.size()) {
        sequence = 0;
    }
    m_player.start(m_tree->sequences[static_cast<usize>(sequence)], static_cast<u32>(sequence));
}

void PowerupCompanion::update(f32 seconds, Action action, bool swung, bool spat) {
    if (m_tree == nullptr) {
        return;
    }
    const auto current = static_cast<s32>(m_player.sequence());
    m_previousFrame = m_player.presentationFrame();
    m_previousGeneration = m_player.generation();
    m_presentationAdvanced = seconds > 0;
    s32 wanted = kReady;
    if (m_kind == Kind::Pojo) {
        wanted = swung ? kAttack : pojoSequenceOf(action);
    } else if (spat && m_tree->sequences.size() > 1) {
        wanted = 1; // the attack its bearer's shot calls for
    }
    // A loop gives way at once; a one-shot plays through before a loop takes over, another
    // one-shot cutting in, and held at its end while it is still asked for (Pojo's death).
    const bool busy = !loops(current) && !m_player.finished();
    if (wanted != current && (!busy || !loops(wanted))) {
        play(wanted);
    }
    m_player.advance(seconds, loops(static_cast<s32>(m_player.sequence())));
    const auto frame = static_cast<s32>(m_player.frame());
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    m_model.setFrame(m_player.sequence(), frame);
    m_textures.advance(seconds);
    m_textures.apply(m_model, *m_tree, m_player.sequence(), frame);
}

std::optional<CompanionVisual> PowerupCompanion::visual(const Mat4& at, f32 alpha) const {
    if (m_tree == nullptr) {
        return std::nullopt;
    }
    return CompanionVisual{m_tree,
                           &m_model,
                           &m_textures,
                           static_cast<u32>(m_kind),
                           at,
                           m_player.sequence(),
                           m_player.generation(),
                           m_player.presentationFrame(),
                           static_cast<f32>(m_textures.frame()) +
                               m_textures.presentationOffset(1).value_or(0),
                           alpha};
}
void PowerupCompanion::draw(RenderDevice& device, const Mat4& clip, const Mat4& at,
                            const WorldLighting& lighting, f32 alpha, const CameraFrame* camera,
                            f32 renderAlpha, TreeModel::Pass pass) const {
    if (m_tree != nullptr) {
        const f32 blend = renderAlpha < 0 || m_presentationAdvanced ? renderAlpha : 1.0f;
        f32 frame = m_player.frame();
        TreePose visualPose;
        const TreePose* pose = &m_pose;
        if (blend >= 0) {
            frame = m_previousGeneration == m_player.generation()
                        ? std::lerp(m_previousFrame, m_player.presentationFrame(),
                                    std::clamp(blend, 0.0f, 1.0f))
                        : m_player.presentationFrame();
            visualPose.evaluate(*m_tree, m_player.sequence(), frame, false, true);
            pose = &visualPose;
        }
        m_model.setPresentationFrame(m_player.sequence(), frame);
        m_textures.apply(m_model, *m_tree, m_player.sequence(), frame,
                         m_textures.presentationOffset(blend));
        m_model.draw(device, clip, at, lighting, pose->matrices(), camera, alpha, pass);
    }
}

void PowerupCompanion::clear() {
    m_kind = Kind::None;
    m_tree = nullptr;
    m_model.clear();
    m_player.stop();
    m_textures.clear();
    m_presentationAdvanced = false;
}

} // namespace gdl::game
