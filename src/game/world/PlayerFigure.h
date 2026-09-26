#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/SoundSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/players/CharacterSave.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/world/PlayerFamiliar.h"

namespace gdl::game {

/** Owns a character's costume, weapon, animation bindings and visual/audio resources.
 * Borrowed missile models and effect archives must not outlive this figure. */
class PlayerFigure {
public:
    PlayerFigure() = default;
    /** Character size shared by body rendering and posed-hand attachments. */
    static f32 bodyScale(const CharacterSave& save, const PowerupEffects& effects);
    // Bound models and animation poses refer into these archives: their owner stays put.
    PlayerFigure(const PlayerFigure&) = delete;
    PlayerFigure& operator=(const PlayerFigure&) = delete;
    PlayerFigure(PlayerFigure&&) = delete;
    PlayerFigure& operator=(PlayerFigure&&) = delete;
    ~PlayerFigure() = default;

    /** Null if the costume cannot be built; missing optional weapons/actions are tolerated. */
    static std::unique_ptr<PlayerFigure> load(RenderDevice& device,
                                              const std::filesystem::path& root,
                                              const CharacterSave& save, bool enter = true);
    static std::filesystem::path costumeDirectory(const std::filesystem::path& root,
                                                  const CharacterSave& save);
    void animate(f32 stickMagnitude, s32 ticks, f32 seconds, PlayerDeed deed = PlayerDeed::None);
    /** Synchronize the temporary companion before animation. The powerup archive must
     * outlive this figure; switching off does not invalidate missiles already in flight. */
    void setCompanionPowerups(RenderDevice& device, ItemArchive& powerups,
                              const Inventory& inventory);
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
              const WorldLighting& lighting, f32 alpha, bool hideWeapon,
              const CameraFrame* camera = nullptr) const;
    /** Retail head equipment, attached to the posed HEAD object. The level's
     * powerup archive must outlive this figure, like its other borrowed draw resources. */
    void drawHeadwear(RenderDevice& device, ItemArchive& powerups, const PowerupEffects& worn,
                      const Mat4& clip, const Mat4& body, const WorldLighting& lighting, f32 alpha);
    /** A posed hand, if available; callers choose their own fallback attachment. */
    std::optional<Vec3> handPosition(const Mat4& body) const;
    std::optional<Mat4> attachment(const Mat4& body, std::string_view objectSuffix) const;
    bool heldWeaponBound() const { return m_handNode >= 0 && m_weapon.bound(); }
    s32 familiarTier() const { return m_familiar.tier(); }
    bool phoenixActive() const { return m_phoenixActive; }
    bool familiarReleased() const { return m_familiarReleased; }
    const TreeModel& familiarMissile() const { return m_familiarMissile; }
    const std::filesystem::path& directory() const { return m_directory; }
    void setStrafe(StrafeWay way) { m_animator.setStrafe(way); }
    void setAttackSpeed(bool rapid, bool speed) { m_animator.setAttackSpeed(rapid, speed); }
    const PlayerAnimator& animator() const { return m_animator; }
    const TreeModel& missile() const { return m_missile; }
    ItemArchive* missileArchive() { return m_missileArchive; }
    std::string_view missileTree() const { return m_missileName; }
    SoundSet& voice() { return m_voice; }
    std::optional<u32> throwSound() const { return m_throwSound; }
    /** Lazily loads the costume colour's effect archive; null when unavailable. */
    ItemArchive* effects();
    /** A temporary full-bright skin; null restores the costume's normal appearance. */
    void setSkinTexture(const Texture* texture) {
        m_model.setMaskedTexture(texture);
        m_model.setAppearance(texture != nullptr);
    }

private:
    /** An unlockable class without its own archive borrows the class it shadows. */
    static std::filesystem::path classFolder(const std::filesystem::path& root, s32 character,
                                             std::string_view sub);
    static std::string_view actionsClassOf(const std::filesystem::path& root, s32 character);
    void loadWeapon(const CharacterSave& save, RenderDevice& device);
    void loadMissile(const std::filesystem::path& root, const CharacterSave& save,
                     RenderDevice& device);
    void loadActions(const std::filesystem::path& root, const CharacterSave& save, bool enter);

    ItemArchive m_costumeArchive;
    AnimationSet m_actions;
    TreeModel m_model;
    PlayerAnimator m_animator;
    const TreeInfo* m_costume = nullptr;
    std::filesystem::path m_directory;
    TreeInfo m_weaponTree;
    TreeModel m_weapon;
    TreeInfo m_headwearTree;
    TreeModel m_headwear;
    s32 m_handNode = -1;
    std::vector<s32> m_classNodeOfNode;
    std::vector<Mat4> m_transforms;
    ItemArchive m_effects;
    PlayerFamiliar m_familiar;
    PlayerFamiliar m_phoenix;
    bool m_phoenixActive = false;
    f32 m_phoenixAlpha = 1;
    TreeModel m_familiarMissile;
    bool m_familiarPending = false;
    bool m_familiarReleased = false;
    TreeModel m_missile;
    ItemArchive* m_missileArchive = nullptr;
    std::string m_missileName;
    SoundSet m_voice;
    std::optional<u32> m_throwSound;
    std::filesystem::path m_effectDirectory;
    bool m_staysInHand = false;
};

} // namespace gdl::game
