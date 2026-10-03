#include "game/world/PlayerFigure.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <span>
#include <string>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "game/players/ClassData.h"
#include "game/players/NameCheats.h"
#include "game/players/Progression.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
namespace {
constexpr std::string_view kPlayersDirectory = "PLAYERS";
constexpr s32 kLevelsPerTier = 10;
constexpr s32 kWeaponTierTwoLevel = 10;
constexpr s32 kWeaponTierThreeLevel = 50;
constexpr std::array<std::string_view, 3> kHandObjects{"R_WRIST", "RIGHTHAN", "RHEND"};
/** The second hand's object, by class (tb_info's second table): what shields hang from. */
constexpr std::array<std::string_view, 2> kArmObjects{"L_WRIST", "LEFTHAND"};
constexpr s32 kValkyrieClass = 1; ///< these two bear shields of their own, on the arm's parent
constexpr s32 kKnightClass = 5;
constexpr s32 kJesterClass = 7; ///< whose hand goes
constexpr std::string_view kHeldWeapon = "WEAP_HOLD";
constexpr std::string_view kClassAnimations = "ANIM";
constexpr std::string_view kSoundDirectory = "audio";
constexpr s32 kOgre = 12;
constexpr f32 kOgreScale = 1.6f;
constexpr f32 kMasterScale = 1.2f;
} // namespace

f32 PlayerFigure::bodyScale(const CharacterSave& save, const PowerupEffects& effects) {
    if (save.character == kOgre) {
        return kOgreScale;
    }
    if (effects.grown()) {
        return PowerupEffects::kGrowthScale;
    }
    return save.progress().appearanceLevel() >= kMaxLevel ? kMasterScale : 1.0f;
}

Mat4 PlayerFigure::bodyPlacement(const Mat4& base, const CharacterSave& save,
                                 const PowerupEffects& effects) {
    const f32 size = bodyScale(save, effects);
    return glm::translate(Mat4{1.0f}, Vec3{0.0f, effects.lift(), 0.0f}) *
           glm::scale(base, Vec3{size, size, size});
}

std::filesystem::path PlayerFigure::costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                     const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    if (const auto* hidden = hiddenCostume(save.name);
        hidden != nullptr && hidden->character == save.character) {
        return unpackedRoot / kPlayersDirectory / cls / hidden->directory;
    }
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path base =
        unpackedRoot / kPlayersDirectory / std::string(cls) / std::string(costume);
    const s32 tier = save.progress().appearanceLevel() / kLevelsPerTier;
    const std::filesystem::path tiered = base.parent_path() / std::format("{}{}0", costume, tier);
    const AssetLocator files(tiered);
    return files.find("objects.ngc") ? files.root() : base;
}

std::unique_ptr<PlayerFigure> PlayerFigure::load(RenderDevice& device,
                                                 const std::filesystem::path& root,
                                                 const CharacterSave& save, bool enter) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path directory = costumeDirectory(root, save);
    auto figure = std::make_unique<PlayerFigure>();
    // Costume HANDGLOW slots are external: JAC/YEL slot 26 names POWERUPS slot 445,
    // not pixels at the costume's local offset or a class SFX texture.
    figure->m_sharedTextureDirectory = root / "POWERUPS";
    if (!figure->m_costumeArchive.models.load(directory) ||
        !figure->m_costumeArchive.textures.load(directory) ||
        !figure->m_costumeArchive.trees.load(directory)) {
        log::warn("Tower: no model for the {} {} under {}", costume, cls, directory.string());
        return nullptr;
    }
    const auto* hidden = hiddenCostume(save.name);
    const std::string_view suffix =
        hidden != nullptr && hidden->character == save.character ? hidden->directory : costume;
    const auto tree = figure->m_costumeArchive.trees.find(std::format("{}_{}", cls, suffix));
    if (!tree.has_value() ||
        !figure->bindModel(figure->m_model, figure->m_costumeArchive.trees.tree(*tree),
                           figure->m_costumeArchive, device)) {
        log::warn("Tower: the {} {} figure could not be built", costume, cls);
        return nullptr;
    }
    figure->m_costume = &figure->m_costumeArchive.trees.tree(*tree);
    const auto animations = figure->m_costumeArchive.trees.textureAnimations();
    if (!animations.empty()) {
        // Costume cycles include hand glows whose frames belong to POWERUPS.
        if (!figure->m_sharedTextures.loaded() &&
            std::ranges::any_of(animations, [](const auto& animation) {
                return animation.cycles() && animation.source < 0;
            })) {
            figure->m_sharedTextures.load(figure->m_sharedTextureDirectory);
        }
        const std::array<TextureSet*, 1> lenders{&figure->m_sharedTextures};
        figure->m_costumeTextures.bind(animations, figure->m_costumeArchive.textures, device,
                                       lenders);
    }
    figure->m_directory = directory;
    figure->m_effectDirectory =
        classFolder(root, save.character, std::format("SFX{}", colorCode(save.color)));
    figure->m_staysInHand = MissileSpec::of(save.character).staysInHand;
    figure->loadWeapon(save, device);
    // InitPlayer's SHADOWL1 node (flags 0x880: no depth write).
    figure->m_shadow.bind(device, figure->m_costumeArchive, kShadowObject);
    figure->loadMissile(root, save, device);
    figure->loadActions(root, save, enter);
    if (PlayerFamiliar::tierFor(save.progress().appearanceLevel()) > 0) {
        ClassDataSet classes;
        classes.load(root / "pdata");
        if (const auto* stats = classes.stats(save.character); stats != nullptr) {
            if (auto* archive = figure->effects(); archive != nullptr) {
                // SFX archives carry named animation slots, not the shared frame pixels.
                // Keep their WEAPONS lender alive as long as the familiar's bound textures.
                figure->m_familiarTextures.load(root / "WEAPONS");
                const std::array<TextureSet*, 1> lenders{&figure->m_familiarTextures};
                figure->m_familiar.bind(device, *archive, save.progress().appearanceLevel(),
                                        stats->familiarOffset, lenders);
                if (const auto shot = archive->trees.find("FAMILIAR_SPIT")) {
                    figure->bindModel(figure->m_familiarMissile, archive->trees.tree(*shot),
                                      *archive, device);
                }
            }
        }
    }
    return figure;
}

void PlayerFigure::loadMissile(const std::filesystem::path& root, const CharacterSave& save,
                               RenderDevice& device) {
    const s32 level = save.progress().appearanceLevel();
    bool inCostume = true;
    const std::string name = MissileSpec::treeName(save.character, level, &inCostume);
    m_missileName = name;
    m_missileArchive = inCostume ? &m_costumeArchive : &m_effects;
    bool bound = false;
    if (inCostume) {
        if (const auto tree = m_costumeArchive.trees.find(name); tree.has_value()) {
            bound =
                bindModel(m_missile, m_costumeArchive.trees.tree(*tree), m_costumeArchive, device);
        }
    } else if (m_effects.load(m_effectDirectory)) {
        if (const auto tree = m_effects.trees.find(name); tree.has_value()) {
            bound = bindModel(m_missile, m_effects.trees.tree(*tree), m_effects, device);
        }
    }
    // InitPlayerMissiles falls back to THROW1 in the costume archive. Several
    // name-selected skins supply that tree instead of their class's THROW0.
    if (!bound) {
        const std::string fallback =
            std::format("{}_THROW1", MissileSpec::of(save.character).model);
        if (const auto tree = m_costumeArchive.trees.find(fallback)) {
            bound =
                bindModel(m_missile, m_costumeArchive.trees.tree(*tree), m_costumeArchive, device);
            if (bound) {
                m_missileName = fallback;
                m_missileArchive = &m_costumeArchive;
            }
        }
    }
    if (!bound) {
        log::warn("Tower: no {} for the {} to throw", name, classCode(save.character));
    }
    // The unlockable classes speak with the voice of the class they shadow.
    const std::string_view voice = classCode(save.character % kStartingClassCount);
    if (m_voice.load(root / kSoundDirectory / voice)) {
        m_throwSound = m_voice.find(std::format("S_{}THROW", voice));
    }
}

void PlayerFigure::loadWeapon(const CharacterSave& save, RenderDevice& device) {
    const s32 level = save.progress().appearanceLevel();
    s32 tier = 1;
    if (level >= kWeaponTierThreeLevel) {
        tier = 3;
    } else if (level >= kWeaponTierTwoLevel) {
        tier = 2;
    }
    std::string weapon{kHeldWeapon};
    if (!m_costumeArchive.models.find(weapon).has_value()) {
        weapon = std::format("WEAP_{}_HD{}", colorCode(save.color), tier);
    }
    if (!m_costumeArchive.models.find(weapon).has_value()) {
        log::warn("Tower: no {} in {}", weapon, m_directory.string());
        return;
    }
    for (usize n = 0; n < m_costume->nodes.size() && m_handNode < 0; ++n) {
        for (const std::string_view suffix : kHandObjects) {
            if (m_costume->nodes[n].object.ends_with(suffix)) {
                m_handNode = static_cast<s32>(n);
                break;
            }
        }
    }
    if (m_handNode < 0) {
        log::warn("Tower: no hand to hold {} in {}", weapon, m_directory.string());
        return;
    }
    m_weaponTree.name = weapon;
    TreeNodeInfo held;
    held.name = weapon;
    held.object = weapon;
    m_weaponTree.nodes.push_back(held);
    if (!bindModel(m_weapon, m_weaponTree, m_costumeArchive, device)) {
        m_handNode = -1;
    }
}

bool PlayerFigure::bindModel(TreeModel& model, const TreeInfo& tree, ItemArchive& archive,
                             RenderDevice& device) {
    if (!m_sharedTextures.loaded()) {
        try {
            const auto needsLender = [&archive](u32 index) {
                return std::ranges::any_of(
                    archive.models.mesh(index).parts, [&archive](const auto& part) {
                        if (part.texture >= archive.textures.size()) {
                            return false;
                        }
                        const auto& texture = archive.textures.entry(part.texture);
                        return texture.external() && !texture.noPicture;
                    });
            };
            bool external = false;
            for (const auto& node : tree.nodes) {
                if (node.name == "DUMMY" || node.name == "NULL1") {
                    continue;
                }
                if (const auto index = archive.models.find(node.object)) {
                    external = external || needsLender(*index);
                }
                for (const auto& run : node.objectFrames) {
                    if (const auto first = archive.models.find(run.object)) {
                        for (s32 frame = 0; frame < run.frames && *first + static_cast<u32>(frame) <
                                                                      archive.models.size();
                             ++frame) {
                            external = external || needsLender(*first + static_cast<u32>(frame));
                        }
                    }
                }
            }
            if (external && !m_sharedTextures.load(m_sharedTextureDirectory)) {
                model.clear();
                return false;
            }
        } catch (const std::exception& error) {
            log::warn("Player figure {}: {}", tree.name, error.what());
            model.clear();
            return false;
        }
    }
    const std::array<TextureSet*, 1> lenders{&m_sharedTextures};
    return model.bind(tree, archive.models, archive.textures, device, lenders);
}

std::filesystem::path PlayerFigure::classFolder(const std::filesystem::path& root, s32 character,
                                                std::string_view sub) {
    const std::filesystem::path players = root / kPlayersDirectory;
    if (const auto found = AssetLocator(players / classCode(character)).find(sub)) {
        return *found;
    }
    return players / classCode(character % kStartingClassCount) / sub;
}

std::string_view PlayerFigure::actionsClassOf(const std::filesystem::path& root, s32 character) {
    const AssetLocator own(root / kPlayersDirectory / classCode(character));
    return classCode(own.find(kClassAnimations) ? character : character % kStartingClassCount);
}

void PlayerFigure::loadActions(const std::filesystem::path& root, const CharacterSave& save,
                               bool enter) {
    const std::string_view cls = actionsClassOf(root, save.character);
    const std::filesystem::path directory = classFolder(root, save.character, kClassAnimations);
    const auto tree = m_actions.load(directory) ? m_actions.find(cls) : std::nullopt;
    if (!tree.has_value() || !m_animator.bind(m_actions.tree(*tree), enter)) {
        log::warn("Tower: no sequences for the {} under {}; the figure stands still", cls,
                  directory.string());
        return;
    }
    m_animator.setCharacter(save.character);
    // The second hand, and what a shield borne on it hides (player.c 4512, 5604).
    m_armNode = -1;
    m_armHidden = -1;
    for (usize n = 0; n < m_costume->nodes.size() && m_armNode < 0; ++n) {
        for (const std::string_view suffix : kArmObjects) {
            if (m_costume->nodes[n].object.ends_with(suffix)) {
                m_armNode = static_cast<s32>(n);
                break;
            }
        }
    }
    if (m_armNode >= 0 && (save.character == kValkyrieClass || save.character == kKnightClass)) {
        m_armHidden = m_costume->nodes[static_cast<usize>(m_armNode)].parent;
    } else if (m_armNode >= 0 && save.character == kJesterClass) {
        m_armHidden = m_armNode;
    }
    // Wings hang from p->node->child->child: the root's first child's first child.
    m_backNode = -1;
    const auto firstChildOf = [this](s32 parent) {
        for (usize n = 0; n < m_costume->nodes.size(); ++n) {
            if (m_costume->nodes[n].parent == parent) {
                return static_cast<s32>(n);
            }
        }
        return -1;
    };
    if (const s32 child = firstChildOf(0); child >= 0) {
        m_backNode = firstChildOf(child);
    }
    const TreeInfo& actions = m_actions.tree(*tree);
    m_classNodeOfNode.clear();
    for (const TreeNodeInfo& node : m_costume->nodes) {
        const auto match = actions.findNode(node.name);
        m_classNodeOfNode.push_back(match.has_value() ? static_cast<s32>(*match) : -1);
    }
    animate(0.0f, 0, 0.0f);
}

void PlayerFigure::animate(f32 stickMagnitude, s32 ticks, f32 seconds, PlayerDeed deed) {
    ++m_animationRevision;
    m_visualTransforms.clear();
    m_costumeTextures.advance(seconds);
    // PlayerMotion consumes the familiar-shot bit on the following simulation step,
    // not on every frame the attack button is held.
    m_familiarReleased = m_familiarPending && deed != PlayerDeed::Die;
    m_familiarPending = false;
    if (!m_animator.bound()) {
        return;
    }
    m_animator.update(PlayerAnimator::motionFor(stickMagnitude), ticks, seconds, deed);
    const bool phoenix = phoenixActive();
    m_familiarPending =
        (phoenix || familiarTier() > 0) &&
        (m_animator.released() || m_animator.strongReleased() || m_animator.superReleased() ||
         m_animator.itemReleased() == PlayerDeed::FireLeft ||
         m_animator.itemReleased() == PlayerDeed::FireRight);
    // Swings and throws animate Pojo; ranged releases animate both firing familiars.
    const bool swung = m_animator.meleeStruck() || m_animator.released();
    m_companion.update(seconds, m_animator.action(), swung, phoenix && m_familiarPending);
    m_familiar.update(seconds, m_familiarPending);
    const std::span<const Mat4> matrices = m_animator.pose().matrices();
    m_transforms.resize(m_costume->nodes.size());
    for (usize n = 0; n < m_transforms.size(); ++n) {
        const s32 source = m_classNodeOfNode[n];
        m_transforms[n] = source >= 0 && static_cast<usize>(source) < matrices.size()
                              ? matrices[static_cast<usize>(source)]
                              : glm::translate(Mat4{1.0f}, m_costume->worldPosition(n));
    }
}

void PlayerFigure::capturePresentation() {
    m_costumeTextures.advance(0);
    m_familiar.capturePresentation();
    m_companion.capturePresentation();
}

void PlayerFigure::updateTrail(const Mat4& body, s32 ticks) {
    const PlayerAnimator::Action action = m_animator.action();
    const bool swinging = action == PlayerAnimator::Action::SlowSwing ||
                          action == PlayerAnimator::Action::Spin ||
                          action == PlayerAnimator::Action::PowerMed;
    const auto hand = static_cast<usize>(std::max(m_handNode, 0));
    const Mat4 wrist = hand < m_transforms.size() ? m_transforms[hand] : Mat4{1.0f};
    m_trail.step(ticks, body * wrist, swinging && heldWeaponBound() && !m_handItemHeld);
}

void PlayerFigure::setCompanionPowerups(RenderDevice& device, ItemArchive& powerups,
                                        const Inventory& inventory, ItemArchive* weapons) {
    const bool shieldRunning = m_animator.action() == PlayerAnimator::Action::ShieldRun;
    m_companion.choose(device,
                       PowerupCompanion::choose(PowerupEffects::of(inventory), shieldRunning),
                       powerups, weapons);
    m_companionAlpha = PowerupCompanion::fadeOf(inventory);
}

ItemArchive* PlayerFigure::effects() {
    if (!m_effects.loaded() && !m_effectDirectory.empty()) {
        m_effects.load(m_effectDirectory);
    }
    return m_effects.loaded() ? &m_effects : nullptr;
}

std::optional<Vec3> PlayerFigure::handPosition(const Mat4& body) const {
    if (m_handNode < 0 || static_cast<usize>(m_handNode) >= m_transforms.size()) {
        return std::nullopt;
    }
    return Vec3{body * m_transforms[static_cast<usize>(m_handNode)] * Vec4{0.0f, 0.0f, 0.0f, 1.0f}};
}

std::optional<Mat4> PlayerFigure::handAttachment(const Mat4& body) const {
    if (m_handNode < 0 || m_costume == nullptr) {
        return std::nullopt;
    }
    const auto hand = static_cast<usize>(m_handNode);
    return body * (hand < m_transforms.size()
                       ? m_transforms[hand]
                       : glm::translate(Mat4{1.0f}, m_costume->worldPosition(hand)));
}

std::optional<Mat4> PlayerFigure::attachment(const Mat4& body,
                                             std::string_view objectSuffix) const {
    if (m_costume != nullptr) {
        for (usize n = 0; n < m_costume->nodes.size() && n < m_transforms.size(); ++n) {
            if (m_costume->nodes[n].object.ends_with(objectSuffix)) {
                return body * m_transforms[n];
            }
        }
    }
    return std::nullopt;
}

void PlayerFigure::preparePresentation(f32 frameBlend) const {
    if (!m_animator.bound() || m_costume == nullptr) {
        m_visualTransforms = m_transforms;
        return;
    }
    m_animator.evaluatePresentation(m_visualPose, frameBlend);
    const auto matrices = m_visualPose.matrices();
    m_visualTransforms.resize(m_costume->nodes.size());
    for (usize n = 0; n < m_visualTransforms.size(); ++n) {
        const s32 source = m_classNodeOfNode[n];
        m_visualTransforms[n] = source >= 0 && static_cast<usize>(source) < matrices.size()
                                    ? matrices[static_cast<usize>(source)]
                                    : glm::translate(Mat4{1.0f}, m_costume->worldPosition(n));
    }
}

std::optional<Mat4> PlayerFigure::visualAttachment(const Mat4& body, s32 node) const {
    const auto& matrices = m_visualTransforms.empty() ? m_transforms : m_visualTransforms;
    if (node < 0 || m_costume == nullptr || static_cast<usize>(node) >= m_costume->nodes.size()) {
        return std::nullopt;
    }
    const auto index = static_cast<usize>(node);
    return body * (index < matrices.size()
                       ? matrices[index]
                       : glm::translate(Mat4{1.0f}, m_costume->worldPosition(index)));
}

std::optional<Mat4> PlayerFigure::visualAttachment(const Mat4& body,
                                                   std::string_view suffix) const {
    if (m_costume != nullptr) {
        for (usize n = 0; n < m_costume->nodes.size(); ++n) {
            if (m_costume->nodes[n].object.ends_with(suffix)) {
                return visualAttachment(body, static_cast<s32>(n));
            }
        }
    }
    return std::nullopt;
}

void PlayerFigure::applyCostumeTextures(TreeModel& model, f32 frameBlend) const {
    // Costume trees have no action sequences. Their free-running cycles must not reset
    // the separately applied hit skin or hide/show state of held equipment.
    const auto offset = m_costumeTextures.presentationOffset(frameBlend);
    for (usize i = 0; i < m_costumeTextures.size(); ++i) {
        if (m_costumeTextures.keyed(i)) {
            continue;
        }
        const auto motion = m_costumeTextures.motion(i, offset);
        if (motion.frame != nullptr) {
            model.setTextureFrame(motion.slot, motion.frame, motion.nextFrame, motion.frameBlend);
        } else {
            Vec2 moved = model.textureOffset(motion.slot);
            const Vec2 direction = m_costumeTextures.scrollDirection(i);
            if (direction.x != 0) {
                moved.x = motion.offset.x;
            }
            if (direction.y != 0) {
                moved.y = motion.offset.y;
            }
            model.setTextureOffset(motion.slot, moved, motion.scale);
        }
    }
}

void PlayerFigure::draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
                        const WorldLighting& lighting, f32 alpha, bool hideWeapon,
                        const CameraFrame* camera, f32 frameBlend) const {
    preparePresentation(frameBlend);
    applyCostumeTextures(m_model, frameBlend);
    applyCostumeTextures(m_weapon, frameBlend);
    m_model.draw(device, clip, body, lighting, m_visualTransforms, nullptr, alpha);
    // The earned familiar is its own skin tree (PlayerProcessSkinFX), beside any companion.
    m_familiar.draw(device, clip, body, lighting, alpha, camera, frameBlend);
    std::optional<Mat4> mount = body;
    switch (PowerupCompanion::mountOf(m_companion.kind())) {
    case PowerupCompanion::Mount::Head: mount = visualAttachment(body, "HEAD"); break;
    case PowerupCompanion::Mount::Back: mount = visualAttachment(body, m_backNode); break;
    case PowerupCompanion::Mount::Body: break;
    }
    if (mount.has_value()) {
        m_companion.draw(device, clip, *mount, lighting, alpha * m_companionAlpha, camera,
                         frameBlend);
    }
    const bool thrown = m_animator.recovering() ||
                        m_animator.action() == PlayerAnimator::Action::StrongThrowRecover;
    if (m_armHeld) {
        if (const auto arm = visualAttachment(body, m_armNode)) {
            m_arm.draw(device, clip, *arm, lighting, {}, nullptr, alpha);
        }
    }
    if (m_handItemHeld) {
        if (const auto hand = visualAttachment(body, m_handNode)) {
            m_handItem.draw(device, clip, *hand, lighting, {}, nullptr, alpha);
        }
    } else if (heldWeaponBound() && !hideWeapon && (!thrown || m_staysInHand)) {
        const auto hand = static_cast<usize>(m_handNode);
        const Mat4 wrist = hand < m_visualTransforms.size()
                               ? m_visualTransforms[hand]
                               : glm::translate(Mat4{1.0f}, m_costume->worldPosition(hand));
        m_weapon.draw(device, clip, body * wrist, lighting, {}, nullptr, alpha);
    }
    if (heldWeaponBound() && !m_handItemHeld) {
        for (const WeaponTrail::Ghost& ghost : m_trail.ghosts()) {
            if (ghost.shown) {
                m_weapon.draw(device, clip, ghost.placement, lighting, {}, nullptr,
                              alpha * ghost.alpha());
            }
        }
    }
}

void PlayerFigure::drawHeadwear(RenderDevice& device, ItemArchive& powerups,
                                const PowerupEffects& worn, const Mat4& clip, const Mat4& body,
                                const WorldLighting& lighting, f32 alpha) {
    // SetPlayerPowerups chooses one head object, in this precedence order.
    std::string_view object;
    if ((worn.special & powerup::kSkorneHorns) != 0) {
        object = "BOSSHORNS";
    } else if ((worn.special & powerup::kSkorneMask) != 0) {
        object = "BOSSMASK";
    } else if ((worn.armor & 0x80000U) != 0) {
        object = "HEAD_HALO";
    } else if ((worn.armor & 0x2000U) != 0) {
        object = "HEAD_GAS";
    } else if (worn.xray()) {
        object = "HEAD_XRAY";
    }
    const auto head = visualAttachment(body, "HEAD");
    if (object.empty() || !head || !powerups.loaded()) {
        return;
    }
    bindObject(device, powerups, object, m_headwearTree, m_headwear);
    m_headwear.draw(device, clip, *head, lighting, {}, nullptr, alpha);
}

void PlayerFigure::drawGem(RenderDevice& device, ItemArchive& powerups, std::string_view object,
                           const Mat4& clip, const Mat4& body, const WorldLighting& lighting,
                           f32 alpha) {
    const auto head = visualAttachment(body, "HEAD");
    if (object.empty() || !head || !powerups.loaded()) {
        return;
    }
    bindObject(device, powerups, object, m_gemTree, m_gem);
    m_gem.draw(device, clip, *head, lighting, {}, nullptr, alpha);
}

void PlayerFigure::bindObject(RenderDevice& device, ItemArchive& archive, std::string_view object,
                              TreeInfo& tree, TreeModel& model) {
    if (tree.name == object) {
        return;
    }
    tree = {};
    tree.name = object;
    TreeNodeInfo node;
    node.name = object;
    node.object = object;
    tree.nodes.push_back(node);
    model.bind(tree, archive.models, archive.textures, device);
}

void PlayerFigure::holdOnArm(RenderDevice& device, ItemArchive* archive, std::string_view object) {
    const bool held = archive != nullptr && archive->loaded() && !object.empty() &&
                      m_armNode >= 0 && archive->models.find(object).has_value();
    if (held && m_armTree.name != object) {
        m_armTree = {};
        m_armTree.name = object;
        TreeNodeInfo node;
        node.name = object;
        node.object = object;
        m_armTree.nodes.push_back(node);
        m_arm.bind(m_armTree, archive->models, archive->textures, device);
    }
    m_armHeld = held && m_arm.bound();
    if (m_armHidden >= 0) {
        m_model.setMeshAlpha(static_cast<usize>(m_armHidden), m_armHeld ? 0.0f : 1.0f);
    }
}

void PlayerFigure::setWeaponPowerups(RenderDevice& device, ItemArchive& powerups,
                                     ItemArchive& weapons, const PowerupEffects& worn) {
    // PlayerProcessPowerups gives the right gauntlet priority over the crossbow and hammer.
    ItemArchive* archive = &weapons;
    std::string_view object;
    if ((worn.special & powerup::kRightGauntlet) != 0) {
        archive = &powerups;
        object = "BOSSGAUNTR";
    } else if ((worn.weapon & powerup::kSuperShot) != 0) {
        object = "SUPERXBOW";
    } else if ((worn.weapon & powerup::kThunderHammer) != 0) {
        object = "HAMMER_HD";
    }
    const bool held = m_handNode >= 0 && archive->loaded() && !object.empty() &&
                      archive->models.find(object).has_value();
    if (held) {
        bindObject(device, *archive, object, m_handItemTree, m_handItem);
    }
    m_handItemHeld = held && m_handItem.bound();
    if (m_handItemHeld) {
        m_trail.clear();
    }
}

std::optional<Mat4> PlayerFigure::armAttachment(const Mat4& body) const {
    const auto arm = static_cast<usize>(m_armNode);
    if (m_armNode < 0 || arm >= m_transforms.size()) {
        return std::nullopt;
    }
    return body * m_transforms[arm];
}

void PlayerFigure::drawMarker(RenderDevice& device, ItemArchive& archive, std::string_view object,
                              const Mat4& clip, const Mat4& body, const WorldLighting& lighting,
                              f32 alpha) {
    if (!archive.loaded() || !archive.models.find(object).has_value()) {
        return;
    }
    if (m_markerTree.name != object) {
        m_markerTree = {};
        m_markerTree.name = object;
        TreeNodeInfo node;
        node.name = object;
        node.object = object;
        m_markerTree.nodes.push_back(node);
        m_marker.bind(m_markerTree, archive.models, archive.textures, device);
    }
    m_marker.draw(device, clip, body, lighting, {}, nullptr, alpha);
}

} // namespace gdl::game
