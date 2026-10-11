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
    configureModel(m_model, tree, archive->models);
    m_textures.bind(archive->trees.textureAnimations(), archive->textures, device);
    play(kReady);
    // Cutscenes can draw immediately after binding, before gameplay advances.
    update(0, Action::Ready, false, false);
}

void PowerupCompanion::configureModel(TreeModel& model, const TreeInfo& tree, ModelSet& models) {
    if (tree.name != "POJO") {
        return;
    }
    // The feather atlas has clear top texels but opaque ones along its bottom.
    // Repeating its 0..1 wing UVs filters the opposite edge into a square border.
    // Generated mip levels also bleed the packed feather regions into clear tips.
    // Keep their native base-level bilinear sampling and clamp only the wings;
    // Pojo's leg/body skins intentionally tile past 1.
    for (const auto& node : tree.nodes) {
        if (!node.object.starts_with("POJOBODY1_L_#") &&
            !node.object.starts_with("POJOBODY1_R_#")) {
            continue;
        }
        if (const auto index = models.find(node.object)) {
            for (const auto& part : models.mesh(*index).parts) {
                model.setTextureSampling(part.texture, true, false);
            }
        }
    }
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
    // Releases are edges, not held actions. A second shot must restart ATTACK even
    // while the previous shot's one-shot is playing; held HIT/DEATH must not restart.
    const bool restart = m_kind == Kind::Pojo && swung;
    if (restart || (wanted != current && (!busy || !loops(wanted)))) {
        play(wanted);
    }
    m_player.advance(seconds, loops(static_cast<s32>(m_player.sequence())));
    const auto frame = static_cast<s32>(m_player.frame());
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    m_model.setFrame(m_player.sequence(), frame);
    m_textures.advance(seconds);
    m_textures.apply(m_model, *m_tree, m_player.sequence(), frame);
}

std::optional<Mat4> PowerupCompanion::attachment(const Mat4& at, std::string_view object) const {
    if (m_tree != nullptr) {
        const auto matrices = m_pose.matrices();
        for (usize node = 0; node < m_tree->nodes.size() && node < matrices.size(); ++node) {
            if (m_tree->nodes[node].object == object) {
                return at * matrices[node];
            }
        }
    }
    return std::nullopt;
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
