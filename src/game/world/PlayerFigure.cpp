#include "game/world/PlayerFigure.h"

#include <array>
#include <format>
#include <span>
#include <string>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/ClassData.h"
#include "game/players/Progression.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
namespace {
constexpr std::string_view kPlayersDirectory = "PLAYERS";
constexpr s32 kLevelsPerTier = 10;
constexpr s32 kWeaponTierTwoLevel = 10;
constexpr s32 kWeaponTierThreeLevel = 50;
constexpr std::array<std::string_view, 3> kHandObjects{"R_WRIST", "RIGHTHAN", "RHEND"};
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

std::filesystem::path PlayerFigure::costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                     const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path base =
        unpackedRoot / kPlayersDirectory / std::string(cls) / std::string(costume);
    const s32 tier = save.progress().appearanceLevel() / kLevelsPerTier;
    const std::filesystem::path tiered = base.parent_path() / std::format("{}{}0", costume, tier);
    return std::filesystem::exists(tiered / "objects.json") ? tiered : base;
}

std::unique_ptr<PlayerFigure> PlayerFigure::load(RenderDevice& device,
                                                 const std::filesystem::path& root,
                                                 const CharacterSave& save, bool enter) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path directory = costumeDirectory(root, save);
    auto figure = std::make_unique<PlayerFigure>();
    if (!figure->m_models.load(directory) || !figure->m_textures.load(directory) ||
        !figure->m_trees.load(directory)) {
        log::warn("Tower: no model for the {} {} under {}", costume, cls, directory.string());
        return nullptr;
    }
    const auto tree = figure->m_trees.find(std::format("{}_{}", cls, costume));
    if (!tree.has_value() || !figure->m_model.bind(figure->m_trees.tree(*tree), figure->m_models,
                                                   figure->m_textures, device)) {
        log::warn("Tower: the {} {} figure could not be built", costume, cls);
        return nullptr;
    }
    figure->m_costume = &figure->m_trees.tree(*tree);
    figure->m_directory = directory;
    figure->m_effectDirectory =
        classFolder(root, save.character, std::format("SFX{}", colorCode(save.color)));
    figure->m_staysInHand = MissileSpec::of(save.character).staysInHand;
    figure->loadWeapon(save, device);
    figure->loadMissile(root, save, device);
    figure->loadActions(root, save, enter);
    if (PlayerFamiliar::tierFor(save.progress().appearanceLevel()) > 0) {
        ClassDataSet classes;
        classes.load(root / "pdata");
        if (const auto* stats = classes.stats(save.character); stats != nullptr) {
            if (auto* archive = figure->effects(); archive != nullptr) {
                figure->m_familiar.bind(device, *archive, save.progress().appearanceLevel(),
                                        stats->familiarOffset);
                if (const auto shot = archive->trees.find("FAMILIAR_SPIT")) {
                    figure->m_familiarMissile.bind(archive->trees.tree(*shot), archive->models,
                                                   archive->textures, device);
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
    bool bound = false;
    if (inCostume) {
        if (const auto tree = m_trees.find(name); tree.has_value()) {
            bound = m_missile.bind(m_trees.tree(*tree), m_models, m_textures, device);
        }
    } else if (m_effects.load(m_effectDirectory)) {
        if (const auto tree = m_effects.trees.find(name); tree.has_value()) {
            bound = m_missile.bind(m_effects.trees.tree(*tree), m_effects.models,
                                   m_effects.textures, device);
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
    if (!m_models.find(weapon).has_value()) {
        weapon = std::format("WEAP_{}_HD{}", colorCode(save.color), tier);
    }
    if (!m_models.find(weapon).has_value()) {
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
    if (!m_weapon.bind(m_weaponTree, m_models, m_textures, device)) {
        m_handNode = -1;
    }
}

std::filesystem::path PlayerFigure::classFolder(const std::filesystem::path& root, s32 character,
                                                std::string_view sub) {
    const std::filesystem::path players = root / kPlayersDirectory;
    std::filesystem::path own = players / classCode(character) / sub;
    if (std::filesystem::exists(own)) {
        return own;
    }
    return players / classCode(character % kStartingClassCount) / sub;
}

std::string_view PlayerFigure::actionsClassOf(const std::filesystem::path& root, s32 character) {
    const std::filesystem::path own =
        root / kPlayersDirectory / classCode(character) / kClassAnimations;
    return classCode(std::filesystem::exists(own) ? character : character % kStartingClassCount);
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
    const TreeInfo& actions = m_actions.tree(*tree);
    m_classNodeOfNode.clear();
    for (const TreeNodeInfo& node : m_costume->nodes) {
        const auto match = actions.findNode(node.name);
        m_classNodeOfNode.push_back(match.has_value() ? static_cast<s32>(*match) : -1);
    }
    animate(0.0f, 0, 0.0f);
}

void PlayerFigure::animate(f32 stickMagnitude, s32 ticks, f32 seconds, PlayerDeed deed) {
    // PlayerMotion consumes the familiar-shot bit on the following simulation step,
    // not on every frame the attack button is held.
    m_familiarReleased = m_familiarPending && deed != PlayerDeed::Die;
    m_familiarPending = false;
    if (!m_animator.bound()) {
        return;
    }
    m_animator.update(PlayerAnimator::motionFor(stickMagnitude), ticks, seconds, deed);
    m_familiarPending =
        familiarTier() > 0 &&
        (m_animator.released() || m_animator.strongReleased() || m_animator.superReleased() ||
         m_animator.itemReleased() == PlayerDeed::FireLeft ||
         m_animator.itemReleased() == PlayerDeed::FireRight);
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

void PlayerFigure::draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
                        const WorldLighting& lighting, f32 alpha, bool hideWeapon) const {
    m_model.draw(device, clip, body, lighting, m_transforms, nullptr, alpha);
    m_familiar.draw(device, clip, body, lighting, alpha);
    const bool thrown = m_animator.recovering() ||
                        m_animator.action() == PlayerAnimator::Action::StrongThrowRecover;
    if (heldWeaponBound() && !hideWeapon && (!thrown || m_staysInHand)) {
        const auto hand = static_cast<usize>(m_handNode);
        const Mat4 wrist = hand < m_transforms.size()
                               ? m_transforms[hand]
                               : glm::translate(Mat4{1.0f}, m_costume->worldPosition(hand));
        m_weapon.draw(device, clip, body * wrist, lighting, {}, nullptr, alpha);
    }
}

} // namespace gdl::game
