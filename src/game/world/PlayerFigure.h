#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"

#include "game/players/CharacterSave.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

/** Owns a character's costume, weapon, animation bindings and visual/audio resources.
 * Borrowed missile models and effect archives must not outlive this figure. */
class PlayerFigure {
public:
    PlayerFigure() = default;
    /** Character size shared by body rendering and posed-hand attachments. */
    static float bodyScale(const CharacterSave& save, const PowerupEffects& effects);
    // Bound models and animation poses refer into these archives: their owner stays put.
    PlayerFigure(const PlayerFigure&) = delete;
    PlayerFigure& operator=(const PlayerFigure&) = delete;
    PlayerFigure(PlayerFigure&&) = delete;
    PlayerFigure& operator=(PlayerFigure&&) = delete;
    ~PlayerFigure() = default;

    /** Null if the costume cannot be built; missing optional weapons/actions are tolerated. */
    static std::unique_ptr<PlayerFigure>
    load(RenderDevice& device, const std::filesystem::path& root, const CharacterSave& save);
    static std::filesystem::path costumeDirectory(const std::filesystem::path& root,
                                                  const CharacterSave& save);
    void animate(float stickMagnitude, std::int32_t ticks, float seconds,
                 PlayerDeed deed = PlayerDeed::None);
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
              const WorldLighting& lighting, float alpha, bool hideWeapon) const;
    /** A posed hand, if available; callers choose their own fallback attachment. */
    std::optional<Vec3> handPosition(const Mat4& body) const;
    bool heldWeaponBound() const { return m_handNode >= 0 && m_weapon.bound(); }
    const std::filesystem::path& directory() const { return m_directory; }
    void setStrafe(StrafeWay way) { m_animator.setStrafe(way); }
    const PlayerAnimator& animator() const { return m_animator; }
    const TreeModel& missile() const { return m_missile; }
    SoundSet& voice() { return m_voice; }
    std::optional<std::uint32_t> throwSound() const { return m_throwSound; }
    /** Lazily loads the costume colour's effect archive; null when unavailable. */
    ItemArchive* effects();

private:
    /** An unlockable class without its own archive borrows the class it shadows. */
    static std::filesystem::path classFolder(const std::filesystem::path& root,
                                             std::int32_t character, std::string_view sub);
    static std::string_view actionsClassOf(const std::filesystem::path& root,
                                           std::int32_t character);
    void loadWeapon(const CharacterSave& save, RenderDevice& device);
    void loadMissile(const std::filesystem::path& root, const CharacterSave& save,
                     RenderDevice& device);
    void loadActions(const std::filesystem::path& root, const CharacterSave& save);

    ModelSet m_models;
    TextureSet m_textures;
    AnimationSet m_trees;
    AnimationSet m_actions;
    TreeModel m_model;
    PlayerAnimator m_animator;
    const TreeInfo* m_costume = nullptr;
    std::filesystem::path m_directory;
    TreeInfo m_weaponTree;
    TreeModel m_weapon;
    std::int32_t m_handNode = -1;
    std::vector<std::int32_t> m_classNodeOfNode;
    std::vector<Mat4> m_transforms;
    ItemArchive m_effects;
    TreeModel m_missile;
    SoundSet m_voice;
    std::optional<std::uint32_t> m_throwSound;
    std::filesystem::path m_effectDirectory;
    bool m_staysInHand = false;
};

} // namespace gdl::game
