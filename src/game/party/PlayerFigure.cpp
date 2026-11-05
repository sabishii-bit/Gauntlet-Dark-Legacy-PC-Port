#include "game/party/PlayerFigure.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <span>
#include <string>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/platform/Paths.h"

#include "game/characters/ClassData.h"
#include "game/characters/NameCheats.h"
#include "game/characters/Progression.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PlayerMissiles.h"
#include "game/players/PlayerDeeds.h"

namespace gdl::game {
namespace {
constexpr std::string_view kPlayersDirectory = "PLAYERS";
constexpr s32 kLevelsPerTier = 10;
constexpr s32 kWeaponTierTwoLevel = 10;
constexpr s32 kWeaponTierThreeLevel = 50;
constexpr std::array<std::string_view, 3> kHandObjects{"R_WRIST", "RIGHTHAN", "RHEND"};
/** The second hand's object, by class (tb_info's second table): what shields hang from. */
constexpr std::array<std::string_view, 2> kArmObjects{"L_WRIST", "LEFTHAND"};
constexpr std::string_view kHeldWeapon = "WEAP_HOLD";
constexpr u32 kHeldWeaponFlags = 0x810; ///< load_player_geo's ordinary held object
constexpr std::string_view kClassAnimations = "ANIM";
constexpr std::string_view kSoundDirectory = "audio";
constexpr f32 kOgreScale = 1.6f;
constexpr f32 kMasterScale = 1.2f;
constexpr std::array kArmEquipment{powerup::kLeftGauntlet, armor::kReflect, armor::kFireArmor,
                                   armor::kElectricArmor};
constexpr std::array<std::string_view, 4> kArmModels{"BOSSGAUNTL", "RF_SHLD", "FW_SHLD", "L_SHLD"};
std::optional<usize> armOf(u32 equipment) {
    for (usize i = 0; i < kArmEquipment.size(); ++i) {
        if ((equipment & kArmEquipment[i]) != 0) {
            return i;
        }
    }
    return std::nullopt;
}

std::string playerResourceKey(const std::filesystem::path& directory) {
    // Only the actual class and bank/costume components identify this binding.
    // Installation paths and on-disk filename casing can differ between peers.
    auto key = std::format("players/{}/{}", paths::genericUtf8(directory.parent_path().filename()),
                           paths::genericUtf8(directory.filename()));
    for (char& value : key) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value + ('a' - 'A'));
        }
    }
    return key;
}
} // namespace

f32 PlayerFigure::bodyScale(const CharacterSave& save, const PowerupEffects& effects) {
    if (save.character == kOgreClass) {
        return kOgreScale;
    }
    if (effects.grown()) {
        return PowerupEffects::kGrowthScale;
    }
    return save.progress().appearanceLevel() >= kMaxLevel ? kMasterScale : 1.0f;
}

Mat4 PlayerFigure::bodyPlacement(const Mat4& base, const CharacterSave& save,
                                 const PowerupEffects& effects) {
    // The lift is in the costume root's own space, under the scaled node
    // (player_motion_phase_exit).
    const f32 size = bodyScale(save, effects);
    return glm::translate(Mat4{1.0f}, Vec3{0.0f, effects.lift() * size, 0.0f}) *
           glm::scale(base, Vec3{size, size, size});
}

/** The costume a save wears beside the colour's: a name's hidden costume, or Sumner's. */
std::string_view costumeOverride(const CharacterSave& save) {
    if (save.character == kSumnerClass) {
        return kSumnerCostume;
    }
    const auto* hidden = hiddenCostume(save.name);
    return hidden != nullptr && hidden->character == save.character ? hidden->directory
                                                                    : std::string_view{};
}

std::filesystem::path PlayerFigure::costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                     const CharacterSave& save) {
    const std::string_view cls = classCode(archiveClassOf(save.character));
    if (const std::string_view override = costumeOverride(save); !override.empty()) {
        return unpackedRoot / kPlayersDirectory / cls / override;
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
                                                 const CharacterSave& save, Entrance entrance) {
    const std::string_view cls = classCode(archiveClassOf(save.character));
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path directory = costumeDirectory(root, save);
    auto figure = std::make_unique<PlayerFigure>();
    // Costume HANDGLOW slots are external: JAC/YEL slot 26 names POWERUPS slot 445,
    // not pixels at the costume's local offset or a class SFX texture.
    figure->m_sharedTextureDirectory = root / "POWERUPS";
    if (!figure->m_costumeArchive.models.load(directory) ||
        !figure->m_costumeArchive.textures.load(directory) ||
        !figure->m_costumeArchive.trees.load(directory)) {
        log::warn("Tower: no model for the {} {} under {}", costume, cls, paths::utf8(directory));
        return nullptr;
    }
    const std::string_view override = costumeOverride(save);
    const std::string_view suffix = override.empty() ? costume : override;
    const auto tree = figure->m_costumeArchive.trees.find(std::format("{}_{}", cls, suffix));
    if (!tree.has_value() ||
        !figure->bindModel(figure->m_model, figure->m_costumeArchive.trees.tree(*tree),
                           figure->m_costumeArchive, device)) {
        log::warn("Tower: the {} {} figure could not be built", costume, cls);
        return nullptr;
    }
    figure->m_costume = &figure->m_costumeArchive.trees.tree(*tree);
    figure->m_directory = directory;
    figure->m_costumeResourceKey = playerResourceKey(directory);
    figure->m_effectDirectory =
        classFolder(root, save.character, std::format("SFX{}", colorCode(save.color)));
    figure->m_effectResourceKey = playerResourceKey(figure->m_effectDirectory);
    const auto animations = figure->m_costumeArchive.trees.textureAnimations();
    if (!animations.empty()) {
        // Hand glows borrow POWERUPS; load_player_geo's special BOMB texmod
        // borrows BOMB00..04 from this player's class SFX archive instead.
        if (std::ranges::any_of(animations, [](const auto& animation) {
                return animation.cycles() && animation.source < 0;
            })) {
            if (!figure->m_sharedTextures.loaded()) {
                figure->m_sharedTextures.load(figure->m_sharedTextureDirectory);
            }
            figure->m_effects.load(figure->m_effectDirectory);
        }
        const std::array<TextureSet*, 2> lenders{&figure->m_sharedTextures,
                                                 &figure->m_effects.textures};
        figure->m_costumeTextures.bind(animations, figure->m_costumeArchive.textures, device,
                                       lenders);
    }
    figure->m_staysInHand = MissileSpec::of(save.character).staysInHand;
    figure->loadWeapon(save, device);
    // InitPlayer's SHADOWL1 node (flags 0x880: no depth write).
    figure->m_shadow.bind(device, figure->m_costumeArchive, kShadowObject);
    figure->loadMissile(root, save, device);
    figure->loadActions(root, save, entrance);
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
                if (figure->m_familiar.bound()) {
                    // Native PlayerMotion emits either familiar spit or Phoenix fire from
                    // the same PDAT origin. Keep both here, with Phoenix across the body
                    // instead of concealing the earned familiar and its projectile.
                    figure->m_phoenixSide = -2.0f * stats->familiarShotOffset.x;
                }
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
    } else if (m_effects.loaded() || m_effects.load(m_effectDirectory)) {
        // Costume animation may already hold pointers to this archive's textures.
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
    // A name-selected costume shipped without a throw (Old Thor's HIR) throws what its
    // class's colour costume throws, from that costume's own archive.
    if (!bound && !costumeOverride(save).empty()) {
        const std::filesystem::path colour = root / kPlayersDirectory /
                                             classCode(archiveClassOf(save.character)) /
                                             colorCode(save.color);
        if (m_missileFallbackArchive.load(colour)) {
            for (const std::string& candidate :
                 {name, std::format("{}_THROW1", MissileSpec::of(save.character).model)}) {
                const auto tree = m_missileFallbackArchive.trees.find(candidate);
                if (tree.has_value() &&
                    bindModel(m_missile, m_missileFallbackArchive.trees.tree(*tree),
                              m_missileFallbackArchive, device)) {
                    bound = true;
                    m_missileName = candidate;
                    m_missileArchive = &m_missileFallbackArchive;
                    break;
                }
            }
        }
    }
    if (!bound) {
        log::warn("Tower: no {} for the {} to throw", name, classCode(save.character));
    }
    // The unlockable classes speak with the voice of the class they shadow.
    const std::string_view voice = classCode(baseClassOf(save.character));
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
        log::warn("Tower: no {} in {}", weapon, paths::utf8(m_directory));
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
        log::warn("Tower: no hand to hold {} in {}", weapon, paths::utf8(m_directory));
        return;
    }
    m_weaponTree.name = weapon;
    TreeNodeInfo held;
    held.name = weapon;
    held.object = weapon;
    held.objectFlags = kHeldWeaponFlags;
    if (baseClassOf(save.character) == kJesterClass) {
        // Native char_type 7 includes the Jackal: the held bomb is a billboard.
        held.objectFlags |= CameraFrame::kFacingFull << CameraFrame::kFacingShift;
    }
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
    if (std::ranges::any_of(tree.nodes, [](const auto& node) { return node.name == "CFGLOW"; })) {
        // load_player_geo adds 0x800810 to the costume's hand glow. Its archive
        // alone supplies the facing mode, but not the additive blend at runtime.
        TreeInfo runtimeTree = tree;
        for (auto& node : runtimeTree.nodes) {
            if (node.name == "CFGLOW") {
                node.objectFlags |= 0x800810;
            }
        }
        return model.bind(runtimeTree, archive.models, archive.textures, device, lenders);
    }
    return model.bind(tree, archive.models, archive.textures, device, lenders);
}

std::filesystem::path PlayerFigure::classFolder(const std::filesystem::path& root, s32 character,
                                                std::string_view sub) {
    // A class's own files, else those of the class it shadows (the unlockables share them).
    const std::filesystem::path players = root / kPlayersDirectory;
    if (const auto found = AssetLocator(players / classCode(archiveClassOf(character))).find(sub)) {
        return *found;
    }
    return players / classCode(baseClassOf(character)) / sub;
}

std::string_view PlayerFigure::actionsClassOf(const std::filesystem::path& root, s32 character) {
    const AssetLocator own(root / kPlayersDirectory / classCode(archiveClassOf(character)));
    return classCode(own.find(kClassAnimations) ? archiveClassOf(character)
                                                : baseClassOf(character));
}

void PlayerFigure::loadActions(const std::filesystem::path& root, const CharacterSave& save,
                               Entrance entrance) {
    const std::string_view cls = actionsClassOf(root, save.character);
    const std::filesystem::path directory = classFolder(root, save.character, kClassAnimations);
    m_actionResourceKey = playerResourceKey(directory);
    const auto tree = m_actions.load(directory) ? m_actions.find(cls) : std::nullopt;
    if (!tree.has_value() || !m_action.animator.bind(m_actions.tree(*tree), entrance)) {
        log::warn("Tower: no sequences for the {} under {}; the figure stands still", cls,
                  paths::utf8(directory));
        return;
    }
    m_action.animator.setCharacter(save.character);
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
    // A borne shield hides the valkyrie's and the knight's own, on the arm's parent, and the
    // jester's hand.
    if (m_armNode >= 0 && (save.character == kValkyrieClass || save.character == kKnightClass)) {
        m_armHidden = m_costume->nodes[static_cast<usize>(m_armNode)].parent;
    } else if (m_armNode >= 0 && save.character == kJesterClass) {
        m_armHidden = m_armNode;
    }
    // Wings hang from p->node->child->child (player.c 5804): p->node is the bare node the
    // costume tree is parented to, its child the tree's root, and that root's first child
    // is the back.
    m_backNode = -1;
    for (usize n = 0; n < m_costume->nodes.size() && m_backNode < 0; ++n) {
        if (m_costume->nodes[n].parent == 0) {
            m_backNode = static_cast<s32>(n);
        }
    }
    const TreeInfo& actions = m_actions.tree(*tree);
    m_actionTree = &actions;
    m_classNodeOfNode.clear();
    for (const TreeNodeInfo& node : m_costume->nodes) {
        const auto match = actions.findNode(node.name);
        m_classNodeOfNode.push_back(match.has_value() ? static_cast<s32>(*match) : -1);
    }
    animate(0.0f, 0, 0.0f);
}

void PlayerFigure::animate(f32 stickMagnitude, s32 ticks, f32 seconds, PlayerDeed deed,
                           bool chainHeldMelee) {
    ++m_action.revision;
    m_visualTransforms.clear();
    m_costumeTextures.advance(seconds);
    // PlayerMotion consumes the familiar-shot bit on the following simulation step,
    // not on every frame the attack button is held.
    m_action.familiarReleased = m_action.familiarPending && deed != PlayerDeed::Die;
    m_action.familiarPending = false;
    if (!m_action.animator.bound()) {
        return;
    }
    m_action.animator.update(PlayerAnimator::motionFor(stickMagnitude), ticks, seconds, deed,
                             chainHeldMelee);
    const bool phoenix = phoenixActive();
    m_action.familiarPending =
        (phoenix || familiarTier() > 0) &&
        (m_action.animator.released() || m_action.animator.strongReleased() ||
         m_action.animator.superReleased() ||
         deedHas(m_action.animator.itemReleased(), deed_trait::kGauntlet));
    // Swings and throws animate Pojo; ranged releases animate both firing familiars.
    const bool swung = m_action.animator.meleeStruck() || m_action.animator.released() ||
                       m_action.animator.strongReleased() || m_action.animator.superReleased();
    m_companion.update(seconds, m_action.animator.action(), swung,
                       phoenix && m_action.familiarPending);
    m_familiar.update(seconds, m_action.familiarPending);
    m_action.transforms.resize(m_costume->nodes.size());
    for (usize n = 0; n < m_action.transforms.size(); ++n) {
        m_action.transforms[n] = poseTransform(n, m_action.animator.pose());
    }
}

void PlayerFigure::animateDeparture(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    ++m_action.revision;
    m_visualTransforms.clear();
    m_costumeTextures.advance(seconds);
    m_action.familiarPending = false;
    m_action.familiarReleased = false;
    m_action.animator.advancePresentation(seconds);
    m_companion.update(seconds, m_action.animator.action(), false, false);
    m_familiar.update(seconds, false);
    if (m_costume && m_action.animator.bound()) {
        m_action.transforms.resize(m_costume->nodes.size());
        for (usize n = 0; n < m_action.transforms.size(); ++n) {
            m_action.transforms[n] = poseTransform(n, m_action.animator.pose());
        }
    }
}

void PlayerFigure::capturePresentation() {
    m_costumeTextures.advance(0);
    m_familiar.capturePresentation();
    m_companion.capturePresentation();
}

void PlayerFigure::updateTrail(const Mat4& body, s32 ticks) {
    const PlayerAnimator::Action action = m_action.animator.action();
    const bool swinging = action == PlayerAnimator::Action::SlowSwing ||
                          action == PlayerAnimator::Action::Spin ||
                          action == PlayerAnimator::Action::PowerMed;
    const auto hand = static_cast<usize>(std::max(m_handNode, 0));
    const Mat4 wrist = hand < m_action.transforms.size() ? m_action.transforms[hand] : Mat4{1.0f};
    m_trail.step(ticks, body * wrist,
                 swinging && heldWeaponBound() && !m_handItemHeld && !pojoActive());
}

void PlayerFigure::setCompanionPowerups(RenderDevice& device, ItemArchive& powerups,
                                        const Inventory& inventory, ItemArchive* weapons) {
    const bool shieldRunning = m_action.animator.action() == PlayerAnimator::Action::ShieldRun;
    m_companion.choose(device,
                       PowerupCompanion::choose(PowerupEffects::of(inventory), shieldRunning),
                       powerups, weapons);
    m_companionAlpha = inventory.permanentPojo ? 1.0f : PowerupCompanion::fadeOf(inventory);
}

bool PlayerFigure::syncCompanionPowerups(const Inventory& inventory) {
    const bool shieldRunning = m_action.animator.action() == PlayerAnimator::Action::ShieldRun;
    const auto kind = PowerupCompanion::choose(PowerupEffects::of(inventory), shieldRunning);
    if (kind != m_companion.kind() ||
        (kind != PowerupCompanion::Kind::None && !m_companion.shown())) {
        return false;
    }
    m_companionAlpha = inventory.permanentPojo ? 1.0f : PowerupCompanion::fadeOf(inventory);
    return true;
}

ItemArchive* PlayerFigure::effects() {
    if (!m_effects.loaded() && !m_effectDirectory.empty()) {
        m_effects.load(m_effectDirectory);
    }
    return m_effects.loaded() ? &m_effects : nullptr;
}

bool PlayerFigure::registerNativeResources(NativeStateResources& resources) const {
    if (!resources.registerArchive(m_costumeArchive, m_costumeResourceKey) ||
        (m_actions.loaded() && !resources.registerAnimations(m_actions, m_actionResourceKey)) ||
        (m_effects.loaded() && !resources.registerArchive(m_effects, m_effectResourceKey)) ||
        (m_sharedTextures.loaded() && !resources.registerTextures(m_sharedTextures, "powerups")) ||
        (m_familiarTextures.loaded() &&
         !resources.registerTextures(m_familiarTextures, "weapons"))) {
        return false;
    }
    // The animator's resolved bank is immutable; its changing clock is native
    // state. Equipment models are mutable draw caches and are not registered.
    return resources.registerTextureAnimator(m_costumeTextures, m_costumeArchive, "costume") &&
           (!m_missile.bound() || ((m_missileArchive != nullptr) &&
                                   resources.registerModel(m_missile, *m_missileArchive,
                                                           "player/missile/" + m_missileName))) &&
           (!m_familiarMissile.bound() ||
            resources.registerModel(m_familiarMissile, m_effects, "player/familiar-missile")) &&
           m_companion.registerNativeResources(resources) && resources.good();
}

std::optional<Vec3> PlayerFigure::handPosition(const Mat4& body) const {
    if (m_handNode < 0 || static_cast<usize>(m_handNode) >= m_action.transforms.size()) {
        return std::nullopt;
    }
    return Vec3{body * m_action.transforms[static_cast<usize>(m_handNode)] *
                Vec4{0.0f, 0.0f, 0.0f, 1.0f}};
}

std::optional<Mat4> PlayerFigure::handAttachment(const Mat4& body) const {
    if (m_handNode < 0 || m_costume == nullptr) {
        return std::nullopt;
    }
    const auto hand = static_cast<usize>(m_handNode);
    return body * (hand < m_action.transforms.size()
                       ? m_action.transforms[hand]
                       : glm::translate(Mat4{1.0f}, m_costume->worldPosition(hand)));
}

Mat4 PlayerFigure::rootAttachment(const Mat4& body) const {
    if (m_costume == nullptr || m_costume->nodes.empty()) {
        return body;
    }
    return body * (m_action.transforms.empty()
                       ? glm::translate(Mat4{1}, m_costume->worldPosition(0))
                       : m_action.transforms.front());
}

Mat4 PlayerFigure::poseTransform(usize node, const TreePose& pose) const {
    if (m_costume == nullptr || node >= m_costume->nodes.size()) {
        return Mat4{1};
    }
    return nodeTransform(node, pose.matrices());
}

Mat4 PlayerFigure::nodeTransform(usize node, std::span<const Mat4> matrices) const {
    Mat4 offset{1};
    s32 current = static_cast<s32>(node);
    for (usize steps = 0; current >= 0 && steps < m_costume->nodes.size(); ++steps) {
        const auto index = static_cast<usize>(current);
        const s32 source = index < m_classNodeOfNode.size() ? m_classNodeOfNode[index] : -1;
        if (source >= 0 && static_cast<usize>(source) < matrices.size()) {
            return matrices[static_cast<usize>(source)] * offset;
        }
        offset = glm::translate(Mat4{1}, m_costume->nodes[index].position) * offset;
        current = m_costume->nodes[index].parent;
    }
    return offset;
}

Mat4 PlayerFigure::rootAttachment(const Mat4& body, const TreePose& pose) const {
    return body * poseTransform(0, pose);
}

std::optional<Mat4> PlayerFigure::attachment(const Mat4& body,
                                             std::string_view objectSuffix) const {
    if (m_costume != nullptr) {
        for (usize n = 0; n < m_costume->nodes.size() && n < m_action.transforms.size(); ++n) {
            if (m_costume->nodes[n].object.ends_with(objectSuffix)) {
                return body * m_action.transforms[n];
            }
        }
    }
    return std::nullopt;
}

std::optional<Mat4> PlayerFigure::attachment(const Mat4& body, std::string_view objectSuffix,
                                             const TreePose& pose) const {
    if (m_costume != nullptr) {
        for (usize node = 0; node < m_costume->nodes.size(); ++node) {
            if (m_costume->nodes[node].object.ends_with(objectSuffix)) {
                return body * poseTransform(node, pose);
            }
        }
    }
    return std::nullopt;
}

void PlayerFigure::preparePresentation(f32 frameBlend) const {
    if (!m_action.animator.bound() || m_costume == nullptr) {
        m_visualTransforms = m_action.transforms;
        return;
    }
    m_action.animator.evaluatePresentation(m_visualPose, frameBlend);
    const auto matrices = m_visualPose.matrices();
    m_visualTransforms.resize(m_costume->nodes.size());
    for (usize n = 0; n < m_visualTransforms.size(); ++n) {
        m_visualTransforms[n] = nodeTransform(n, matrices);
    }
}

std::optional<Mat4> PlayerFigure::visualAttachment(const Mat4& body, s32 node) const {
    const auto& matrices = m_visualTransforms.empty() ? m_action.transforms : m_visualTransforms;
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
                        const CameraFrame* camera, f32 frameBlend, bool handOccupied,
                        TreeModel::Pass companionPass) const {
    preparePresentation(frameBlend);
    // PlayerProcessPowerups hides the original costume subtree while POJO is
    // attached to its root. That includes its held weapon, arm items and trails.
    if (pojoActive()) {
        drawCompanions(device, clip, body, lighting, alpha, camera, frameBlend, companionPass);
        return;
    }
    applyCostumeTextures(m_model, frameBlend);
    applyCostumeTextures(m_weapon, frameBlend);
    m_model.setHighlight(m_highlight);
    m_model.draw(device, clip, body, lighting, m_visualTransforms, camera, alpha);
    m_model.setHighlight(nullptr);
    drawCompanions(device, clip, body, lighting, alpha, camera, frameBlend, companionPass);
    const bool thrown = m_action.animator.recovering() ||
                        m_action.animator.action() == PlayerAnimator::Action::StrongThrowRecover;
    if (m_armHeld) {
        if (const auto arm = visualAttachment(body, m_armNode)) {
            m_arm.draw(device, clip, *arm, lighting, {}, nullptr, alpha);
        }
    }
    // A separately drawn relic replaces both the class weapon and hand powerups.
    // The other arm, costume and companions remain visible.
    if (handOccupied) {
        return;
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
        m_weapon.draw(device, clip, body * wrist, lighting, {}, camera, alpha);
    }
    if (heldWeaponBound() && !m_handItemHeld) {
        for (const WeaponTrail::Ghost& ghost : m_trail.ghosts()) {
            if (ghost.shown) {
                m_weapon.draw(device, clip, ghost.placement, lighting, {}, camera,
                              alpha * ghost.alpha());
            }
        }
    }
}

void PlayerFigure::drawPose(RenderDevice& device, const Mat4& clip, const Mat4& body,
                            const WorldLighting& lighting, const TreePose& pose,
                            PlayerAnimator::Action action, f32 textureFrame,
                            const Texture* hitFlash, const CameraFrame* camera,
                            TreeModel::Pass pass, bool hideWeapon, u32 armEquipment) const {
    if (m_costume == nullptr || !pose.posed()) {
        return;
    }
    const auto matrices = pose.matrices();
    std::vector<Mat4> transforms(m_costume->nodes.size());
    for (usize node = 0; node < transforms.size(); ++node) {
        transforms[node] = nodeTransform(node, matrices);
    }
    // Absolute display texture clock relative to this figure's bind. Does not
    // advance either the local texture clock or the gameplay animator.
    const f32 offset = textureFrame - static_cast<f32>(m_costumeTextures.frame());
    m_costumeTextures.apply(m_model, offset);
    m_costumeTextures.apply(m_weapon, offset);
    m_model.setMaskedTexture(hitFlash);
    m_model.setAppearance({.unlit = hitFlash != nullptr});
    const auto arm = armEquipmentReady(armEquipment) ? armOf(armEquipment) : std::nullopt;
    const bool armHeld = arm && m_armNode >= 0 && static_cast<usize>(m_armNode) < transforms.size();
    if (m_armHidden >= 0) {
        m_model.setMeshAlpha(static_cast<usize>(m_armHidden), armHeld ? 0.0f : 1.0f);
    }
    m_model.draw(device, clip, body, lighting, transforms, camera, 1, pass);
    if (armHeld) {
        m_replicaArms[*arm].model.draw(device, clip,
                                       body * transforms[static_cast<usize>(m_armNode)], lighting,
                                       {}, nullptr, 1, pass);
    }
    const bool thrown = action == PlayerAnimator::Action::ThrowRecover ||
                        action == PlayerAnimator::Action::ThrowMovingRecover ||
                        action == PlayerAnimator::Action::StrongThrowRecover;
    if (heldWeaponBound() && !hideWeapon && (!thrown || m_staysInHand)) {
        m_weapon.draw(device, clip, body * transforms[static_cast<usize>(m_handNode)], lighting, {},
                      camera, 1, pass);
    }
    m_model.setMaskedTexture(nullptr);
    m_model.setAppearance({});
    if (m_armHidden >= 0) {
        m_model.setMeshAlpha(static_cast<usize>(m_armHidden), m_armHeld ? 0.0f : 1.0f);
    }
}

bool PlayerFigure::bindArmEquipment(RenderDevice& device, ItemArchive& powerups,
                                    ItemArchive& weapons) {
    for (usize i = 0; i < m_replicaArms.size(); ++i) {
        auto& archive = i == 0 ? powerups : weapons;
        if (!archive.loaded() || !archive.models.find(kArmModels[i])) {
            return false;
        }
    }
    // Unlike the native held-item cache, this catalog can be rebound to another
    // trusted archive before receiving packets. Equal model names are not equal owners.
    for (auto& arm : m_replicaArms) {
        arm.model.clear();
        arm.tree = {};
    }
    for (usize i = 0; i < m_replicaArms.size(); ++i) {
        auto& archive = i == 0 ? powerups : weapons;
        auto& arm = m_replicaArms[i];
        bindObject(device, archive, kArmModels[i], arm.tree, arm.model);
        if (!arm.model.bound()) {
            for (auto& bound : m_replicaArms) {
                bound.model.clear();
                bound.tree = {};
            }
            return false;
        }
    }
    return true;
}

bool PlayerFigure::armEquipmentReady(u32 equipment) const {
    if ((equipment & ~(powerup::kLeftGauntlet | armor::kArmShields)) != 0) {
        return false;
    }
    const auto arm = armOf(equipment);
    return !arm || m_replicaArms[*arm].model.bound();
}

std::array<std::optional<CompanionVisual>, 2> PlayerFigure::companionVisuals(const Mat4& body,
                                                                             f32 alpha) const {
    std::array<std::optional<CompanionVisual>, 2> result;
    result[0] = m_familiar.visual(body, alpha);
    if (!m_companion.shown()) {
        return result;
    }
    preparePresentation(1);
    std::optional<Mat4> mount = phoenixActive() ? phoenixAttachment(body) : body;
    switch (PowerupCompanion::mountOf(m_companion.kind())) {
    case PowerupCompanion::Mount::Head: mount = visualAttachment(body, "HEAD"); break;
    case PowerupCompanion::Mount::Back: mount = visualAttachment(body, m_backNode); break;
    case PowerupCompanion::Mount::Body: break;
    }
    if (mount) {
        result[1] = m_companion.visual(*mount, alpha * m_companionAlpha);
    }
    return result;
}
void PlayerFigure::drawCompanions(RenderDevice& device, const Mat4& clip, const Mat4& body,
                                  const WorldLighting& lighting, f32 alpha,
                                  const CameraFrame* camera, f32 frameBlend,
                                  TreeModel::Pass pass) const {
    // Earned familiars have their own materials too. In particular, the Archer's
    // butterfly does not write depth; drawing it before deferred scenery lets
    // even background geometry paint over it. Keep its authored state and defer it.
    m_familiar.draw(device, clip, body, lighting, alpha, camera, frameBlend, pass);
    if (!m_companion.shown()) {
        return;
    }
    const auto mountKind = PowerupCompanion::mountOf(m_companion.kind());
    if (mountKind != PowerupCompanion::Mount::Body) {
        preparePresentation(frameBlend);
    }
    std::optional<Mat4> mount = phoenixActive() ? phoenixAttachment(body) : body;
    switch (mountKind) {
    case PowerupCompanion::Mount::Head: mount = visualAttachment(body, "HEAD"); break;
    case PowerupCompanion::Mount::Back: mount = visualAttachment(body, m_backNode); break;
    case PowerupCompanion::Mount::Body: break;
    }
    if (mount) {
        m_companion.draw(device, clip, *mount, lighting, alpha * m_companionAlpha, camera,
                         frameBlend, pass);
    }
}

void PlayerFigure::drawHeadwear(RenderDevice& device, ItemArchive& powerups,
                                const PowerupEffects& worn, const Mat4& clip, const Mat4& body,
                                const WorldLighting& lighting, f32 alpha) {
    if (pojoActive()) {
        return;
    }
    // SetPlayerPowerups chooses one head object, in this precedence order.
    std::string_view object;
    if ((worn.special & powerup::kSkorneHorns) != 0) {
        object = "BOSSHORNS";
    } else if ((worn.special & powerup::kSkorneMask) != 0) {
        object = "BOSSMASK";
    } else if ((worn.armor & armor::kAntiDeath) != 0) {
        object = "HEAD_HALO";
    } else if ((worn.armor & armor::kImmuneGas) != 0) {
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
    if (pojoActive()) {
        return;
    }
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
    } else if ((worn.weapon & damage::kSuperShot) != 0) {
        object = "SUPERXBOW";
    } else if ((worn.weapon & damage::kHammer) != 0) {
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
    if (m_armNode < 0 || arm >= m_action.transforms.size()) {
        return std::nullopt;
    }
    return body * m_action.transforms[arm];
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

void PlayerFigure::saveLocal(LocalSnapshot& state) const {
    state.m_actions = m_action;
    m_companion.saveLocal(state.m_companion);
    m_familiar.saveLocal(state.m_familiar);
    state.m_costumeTextures = m_costumeTextures.clock();
    state.m_companionAlpha = m_companionAlpha;
    state.m_owner = m_localOwner;
}

bool PlayerFigure::appendState(NativeStateDigest& digest, const ActionState& actions,
                               const TextureAnimator::Clock& textures, f32 companionAlpha) const {
    if (!m_costumeArchive.loaded() || !m_costume) {
        digest.invalidate();
        return false;
    }
    digest.string("PlayerFigure.native.1");
    digest.resource(&m_costumeArchive);
    digest.resource(m_costume);
    digest.resource(m_actionTree);
    // These immutable bindings control native hand/head attachment evaluation;
    // peers may own separate allocations of the same authored class/costume.
    digest.values(m_handNode, m_armNode, m_armHidden, m_backNode, m_classNodeOfNode, m_staysInHand,
                  m_phoenixSide);
    m_costumeTextures.appendBindingDigest(digest);
    digest.fields<5>(actions);
    digest.fields<3>(textures);
    digest.value(companionAlpha);
    return digest.good();
}

bool PlayerFigure::appendNativeDigest(NativeStateDigest& digest) const {
    return appendState(digest, m_action, m_costumeTextures.clock(), m_companionAlpha) &&
           m_companion.appendNativeDigest(digest) && m_familiar.appendNativeDigest(digest);
}

bool PlayerFigure::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [actions, companion, familiar, textures, companionAlpha, owner] = saved;
    return appendState(digest, actions, textures, companionAlpha) &&
           m_companion.appendLocalDigest(digest, companion) &&
           m_familiar.appendLocalDigest(digest, familiar);
}

bool PlayerFigure::canRestoreLocal(const LocalSnapshot& state) const {
    return state.m_owner.lock() == m_localOwner && m_companion.canRestoreLocal(state.m_companion) &&
           m_familiar.canRestoreLocal(state.m_familiar);
}

bool PlayerFigure::restoreLocal(const LocalSnapshot& state) {
    if (!canRestoreLocal(state)) {
        return false;
    }
    m_action = state.m_actions;
    m_companion.restoreLocal(state.m_companion);
    m_familiar.restoreLocal(state.m_familiar);
    m_costumeTextures.restoreClock(state.m_costumeTextures);
    m_companionAlpha = state.m_companionAlpha;
    m_visualTransforms.clear();
    m_trail.clear(); // never draw speculative ghosts from the discarded branch
    return true;
}

} // namespace gdl::game
