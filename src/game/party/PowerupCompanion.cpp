#include "game/party/PowerupCompanion.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {

bool PowerupCompanion::appendState(NativeStateDigest& digest, const State& state) const {
    const auto& [kind, resource, poseTree, pose, player, textures, previousFrame,
                 previousGeneration, presentationAdvanced] = state;
    digest.string("PowerupCompanion.native.1");
    digest.value(kind);
    digest.flag(resource != nullptr);
    if (resource != nullptr) {
        const bool owned = std::ranges::any_of(m_resources->entries, [resource](const auto& entry) {
            return entry.get() == resource;
        });
        if (!owned || resource->archive == nullptr || !resource->archive->loaded()) {
            digest.invalidate();
            return false;
        }
        const auto& [archive, tree, model, bank] = *resource;
        digest.resource(archive);
        digest.resource(tree);
        digest.flag(model.bound());
        bank.appendBindingDigest(digest);
    }
    digest.resource(poseTree);
    digest.values(pose, player);
    digest.fields<3>(textures);
    digest.values(previousFrame, previousGeneration, presentationAdvanced);
    return digest.good();
}

bool PowerupCompanion::appendNativeDigest(NativeStateDigest& digest) const {
    return appendState(digest, m_state);
}

bool PowerupCompanion::appendLocalDigest(NativeStateDigest& digest,
                                         const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner] = saved;
    return appendState(digest, state);
}

bool PowerupCompanion::registerNativeResources(NativeStateResources& registry) const {
    // A discarded kind change keeps these banks for saved poses and replay.
    // Cache population itself does not enter the native state digest.
    for (const auto& resource : m_resources->entries) {
        if (!resource || !resource->archive || !registry.refreshArchive(*resource->archive)) {
            return false;
        }
    }
    return registry.good();
}

namespace {

using Action = PlayerAnimator::Action;

/** The specials that bring a companion (0x7004F1), whose time left fades it. */
constexpr u32 kCompanionSpecials = powerup::kLevitation | powerup::kBreath | powerup::kPhoenix |
                                   powerup::kPojo | powerup::kMikey | powerup::kHandOfDeath |
                                   powerup::kHealthVamp;
constexpr f32 kFadeSeconds = 1.0f;

} // namespace

PowerupCompanion::Kind PowerupCompanion::choose(const PowerupEffects& worn, bool shieldRunning) {
    if ((worn.special & powerup::kPojo) != 0) {
        return Kind::Pojo;
    }
    if ((worn.armor & armor::kFireArmor) != 0 && shieldRunning) {
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
            (slot.kind == powerup::kArmor && (slot.flags & armor::kFireArmor) != 0);
        if (brings) {
            left = std::max(left, slot.strength < 0.0f ? kFadeSeconds : slot.strength);
        }
    }
    return left < 0.0f ? 1.0f : std::clamp(left / kFadeSeconds, 0.0f, 1.0f);
}

void PowerupCompanion::choose(RenderDevice& device, Kind kind, ItemArchive& powerups,
                              ItemArchive* weapons) {
    if (kind == m_state.kind && (kind == Kind::None || shown())) {
        return;
    }
    clear();
    m_state.kind = kind;
    ItemArchive* archive = fromWeapons(kind) ? weapons : &powerups;
    if (kind == Kind::None || archive == nullptr || !archive->loaded()) {
        return;
    }
    const auto index = archive->trees.find(treeOf(kind));
    if (!index.has_value()) {
        return;
    }
    const TreeInfo& tree = archive->trees.tree(*index);
    if (tree.sequences.empty()) {
        return;
    }
    Resource* resource = findResource(*archive, tree);
    if (!resource) {
        auto loaded = std::make_unique<Resource>();
        loaded->archive = archive;
        loaded->tree = &tree;
        if (!loaded->model.bind(tree, archive->models, archive->textures, device)) {
            return;
        }
        configureModel(loaded->model, tree, archive->models);
        loaded->textures.bind(archive->trees.textureAnimations(), archive->textures, device);
        resource = loaded.get();
        m_resources->entries.push_back(std::move(loaded));
    }
    m_state.resource = resource;
    play(kReady);
    // Cutscenes can draw immediately after binding, before gameplay advances.
    update(0, Action::Ready, false, false);
}

PowerupCompanion::Resource* PowerupCompanion::findResource(ItemArchive& archive,
                                                           const TreeInfo& tree) const {
    for (const auto& resource : m_resources->entries) {
        if (resource->archive == &archive && resource->tree == &tree) {
            return resource.get();
        }
    }
    return nullptr;
}

void PowerupCompanion::saveLocal(LocalSnapshot& state) const {
    state.m_state = m_state;
    state.m_owner = m_resources;
}

bool PowerupCompanion::canRestoreLocal(const LocalSnapshot& state) const {
    return state.m_owner.lock() == m_resources;
}

bool PowerupCompanion::restoreLocal(const LocalSnapshot& state) {
    if (!canRestoreLocal(state)) {
        return false;
    }
    m_state = state.m_state;
    ++m_bindingRevision; // invalidate legacy prepared restores from the abandoned branch
    return true;
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
    const auto* currentTree = tree();
    if (!currentTree) {
        return;
    }
    if (sequence < 0 || static_cast<usize>(sequence) >= currentTree->sequences.size()) {
        sequence = 0;
    }
    m_state.player.start(currentTree->sequences[static_cast<usize>(sequence)],
                         static_cast<u32>(sequence));
}

void PowerupCompanion::update(f32 seconds, Action action, bool swung, bool spat) {
    const auto* currentTree = tree();
    if (!currentTree) {
        return;
    }
    auto& player = m_state.player;
    auto& resource = *m_state.resource;
    const auto current = static_cast<s32>(player.sequence());
    m_state.previousFrame = player.presentationFrame();
    m_state.previousGeneration = player.generation();
    m_state.presentationAdvanced = seconds > 0;
    s32 wanted = kReady;
    if (m_state.kind == Kind::Pojo) {
        wanted = swung ? kAttack : pojoSequenceOf(action);
    } else if (spat && currentTree->sequences.size() > 1) {
        wanted = 1; // the attack its bearer's shot calls for
    }
    // A loop gives way at once; a one-shot plays through before a loop takes over, another
    // one-shot cutting in, and held at its end while it is still asked for (Pojo's death).
    const bool busy = !loops(current) && !player.finished();
    // Releases are edges, not held actions. A second shot must restart ATTACK even
    // while the previous shot's one-shot is playing; held HIT/DEATH must not restart.
    const bool restart = m_state.kind == Kind::Pojo && swung;
    if (restart || (wanted != current && (!busy || !loops(wanted)))) {
        play(wanted);
    }
    player.advance(seconds, loops(static_cast<s32>(player.sequence())));
    const auto frame = static_cast<s32>(player.frame());
    m_state.pose.evaluate(*currentTree, player.sequence(), player.frame());
    m_state.poseTree = currentTree;
    resource.model.setFrame(player.sequence(), frame);
    resource.textures.restoreClock(m_state.textures);
    resource.textures.advance(seconds);
    m_state.textures = resource.textures.clock();
    resource.textures.apply(resource.model, *currentTree, player.sequence(), frame);
}

std::optional<Mat4> PowerupCompanion::attachment(const Mat4& at, std::string_view object) const {
    if (const auto* currentTree = tree()) {
        const auto matrices = m_state.pose.matrices();
        for (usize node = 0; node < currentTree->nodes.size() && node < matrices.size(); ++node) {
            if (currentTree->nodes[node].object == object) {
                return at * matrices[node];
            }
        }
    }
    return std::nullopt;
}

std::optional<CompanionVisual> PowerupCompanion::visual(const Mat4& at, f32 alpha) const {
    if (!shown()) {
        return std::nullopt;
    }
    auto& resource = *m_state.resource;
    resource.textures.restoreClock(m_state.textures);
    return CompanionVisual{tree(),
                           &resource.model,
                           &resource.textures,
                           static_cast<u32>(m_state.kind),
                           at,
                           m_state.player.sequence(),
                           m_state.player.generation(),
                           m_state.player.presentationFrame(),
                           static_cast<f32>(resource.textures.frame()) +
                               resource.textures.presentationOffset(1).value_or(0),
                           alpha};
}
void PowerupCompanion::draw(RenderDevice& device, const Mat4& clip, const Mat4& at,
                            const WorldLighting& lighting, f32 alpha, const CameraFrame* camera,
                            f32 renderAlpha, TreeModel::Pass pass) const {
    if (const auto* currentTree = tree()) {
        const auto& player = m_state.player;
        auto& resource = *m_state.resource;
        const f32 blend = renderAlpha < 0 || m_state.presentationAdvanced ? renderAlpha : 1.0f;
        f32 frame = player.frame();
        TreePose visualPose;
        const TreePose* pose = &m_state.pose;
        if (blend >= 0) {
            frame = m_state.previousGeneration == player.generation()
                        ? portable::lerp(m_state.previousFrame, player.presentationFrame(),
                                         std::clamp(blend, 0.0f, 1.0f))
                        : player.presentationFrame();
            visualPose.evaluate(*currentTree, player.sequence(), frame, false, true);
            pose = &visualPose;
        }
        resource.model.setPresentationFrame(player.sequence(), frame);
        resource.textures.restoreClock(m_state.textures);
        resource.textures.apply(resource.model, *currentTree, player.sequence(), frame,
                                resource.textures.presentationOffset(blend));
        resource.model.draw(device, clip, at, lighting, pose->matrices(), camera, alpha, pass);
    }
}

void PowerupCompanion::clear() {
    ++m_bindingRevision;
    m_state.kind = Kind::None;
    m_state.resource = nullptr;
    m_state.player.stop();
    m_state.textures = {};
    m_state.presentationAdvanced = false;
}

bool PowerupCompanion::Checkpoint::valid() const {
    return kind >= Kind::None && kind <= Kind::Wings &&
           tree.size() <= TreePose::Checkpoint::kMaxName && tree.find('\0') == std::string::npos &&
           animation.valid() && textures.valid() && (!pose || pose->valid()) &&
           std::isfinite(previousFrame) && previousFrame >= 0 &&
           previousFrame <= AnimationPlayer::kCheckpointFrames &&
           previousGeneration <= animation.generation && tree.empty() != animation.playing &&
           (tree.empty() ||
            (kind != Kind::None && tree == treeOf(kind) && pose && pose->tree == tree));
}
std::optional<PowerupCompanion::Checkpoint> PowerupCompanion::checkpoint() const {
    Checkpoint state{m_state.kind,
                     tree() ? tree()->name : std::string{},
                     m_state.player.checkpoint(),
                     m_state.textures,
                     m_state.pose.checkpoint(),
                     m_state.previousFrame,
                     m_state.previousGeneration,
                     m_state.presentationAdvanced};
    if ((m_state.pose.posed() && !state.pose) || !state.valid()) {
        return std::nullopt;
    }
    return state;
}
bool PowerupCompanion::restore(const Checkpoint& state) {
    const auto* currentTree = tree();
    if (!state.valid() || state.kind != m_state.kind ||
        state.tree != (currentTree ? currentTree->name : std::string{}) ||
        (state.pose && m_state.poseTree == nullptr)) {
        return false;
    }
    const TreeSequenceInfo* sequence = nullptr;
    if (state.animation.playing) {
        if (!currentTree || state.animation.sequence >= currentTree->sequences.size()) {
            return false;
        }
        sequence = &currentTree->sequences[state.animation.sequence];
    }
    auto next = m_state;
    if (!next.player.restore(state.animation, sequence) ||
        (state.pose && !next.pose.restore(*m_state.poseTree, *state.pose))) {
        return false;
    }
    if (!state.pose) {
        next.pose = {};
        next.poseTree = nullptr;
    }
    next.textures = state.textures;
    m_state = std::move(next);
    m_state.previousFrame = state.previousFrame;
    m_state.previousGeneration = state.previousGeneration;
    m_state.presentationAdvanced = state.presentationAdvanced;
    return true;
}

} // namespace gdl::game
