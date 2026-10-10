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
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/players/CharacterSave.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/world/BlobShadow.h"
#include "game/world/PlayerFamiliar.h"
#include "game/world/PowerupCompanion.h"
#include "game/world/WeaponTrail.h"

namespace gdl::game {

/** Owns a character's costume, weapon, animation bindings and visual/audio resources.
 * Borrowed missile models and effect archives must not outlive this figure. */
class PlayerFigure {
public:
    static constexpr std::string_view kShadowObject = "SHADOWL1";
    PlayerFigure() = default;
    /** Character size shared by body rendering and posed-hand attachments. */
    static f32 bodyScale(const CharacterSave& save, const PowerupEffects& effects);
    /** Where the body is drawn and its parts hang: `base` (the actor's transform, or the
     * capture's) at `bodyScale`, lifted by what the powerups hold it up by. */
    static Mat4 bodyPlacement(const Mat4& base, const CharacterSave& save,
                              const PowerupEffects& effects);
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
    /** Clears the last presentation interval when a scene tick may hold the figure. */
    void capturePresentation();
    u64 animationRevision() const { return m_animationRevision; }
    /** Synchronize the temporary companion before animation (the fire shield's blaze comes
     * from `weapons`). The archives must outlive this figure; switching off does not
     * invalidate missiles already in flight. */
    void setCompanionPowerups(RenderDevice& device, ItemArchive& powerups,
                              const Inventory& inventory, ItemArchive* weapons = nullptr);
    bool hasShadow() const { return m_shadow.bound(); }
    /** The costume's shadow, unscaled, lying on the floor at `ground` along its `normal`. */
    void drawShadow(RenderDevice& device, const Mat4& clip, const Vec3& eye, const Vec3& ground,
                    const Vec3& normal, const WorldLighting& lighting, f32 alpha) const {
        m_shadow.draw(device, clip, eye, ground, normal, lighting, alpha);
    }
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
              const WorldLighting& lighting, f32 alpha, bool hideWeapon,
              const CameraFrame* camera = nullptr, f32 frameBlend = 1.0f, bool handOccupied = false,
              TreeModel::Pass companionPass = TreeModel::Pass::All) const;
    /** Base costume and ordinary held weapon at an externally sampled class pose.
     * No animator/companion updates, gameplay attachments, trails or release events.
     * Powerups, familiars and transient effects require their own replicated state. */
    void drawPose(RenderDevice& device, const Mat4& clip, const Mat4& body,
                  const WorldLighting& lighting, const TreePose& pose,
                  PlayerAnimator::Action action, f32 textureFrame, const Texture* hitFlash,
                  const CameraFrame* camera, TreeModel::Pass pass = TreeModel::Pass::All) const;
    const TreeInfo* actionTree() const { return m_actionTree; }
    /** Companion effects can composite after scenery without redrawing the body or held gear. */
    void drawCompanions(RenderDevice& device, const Mat4& clip, const Mat4& body,
                        const WorldLighting& lighting, f32 alpha, const CameraFrame* camera,
                        f32 frameBlend, TreeModel::Pass pass) const;
    /** Retail head equipment, attached to the posed HEAD object. The level's
     * powerup archive must outlive this figure, like its other borrowed draw resources. */
    void drawHeadwear(RenderDevice& device, ItemArchive& powerups, const PowerupEffects& worn,
                      const Mat4& clip, const Mat4& body, const WorldLighting& lighting, f32 alpha);
    /** The gem `object` of the powerup archive on the head beside the headwear (gem_object on
     * the head node), none when empty; the archive must outlive this figure. */
    void drawGem(RenderDevice& device, ItemArchive& powerups, std::string_view object,
                 const Mat4& clip, const Mat4& body, const WorldLighting& lighting, f32 alpha);
    /** An object of `archive` hung on the body's root, as the sign of who is it; the archive
     * must outlive this figure. */
    void drawMarker(RenderDevice& device, ItemArchive& archive, std::string_view object,
                    const Mat4& clip, const Mat4& body, const WorldLighting& lighting, f32 alpha);
    /** Bears `object` of `archive` on the second hand (a shield, the left gauntlet) from now
     * on, none when empty; the valkyrie's and knight's own shield and the jester's hand are
     * hidden meanwhile (PlayerProcessPowerups). The archive must outlive this figure. */
    void holdOnArm(RenderDevice& device, ItemArchive* archive, std::string_view object);
    /** Replaces the normal hand weapon with the equipped gauntlet, crossbow or hammer.
     * Both archives must outlive this figure's borrowed models. */
    void setWeaponPowerups(RenderDevice& device, ItemArchive& powerups, ItemArchive& weapons,
                           const PowerupEffects& worn);
    /** Where the second hand is, when the body has one. */
    std::optional<Mat4> armAttachment(const Mat4& body) const;
    /** A posed hand, if available; callers choose their own fallback attachment. */
    std::optional<Vec3> handPosition(const Mat4& body) const;
    /** The weapon hand's world transform, as the held weapon is drawn by it. */
    std::optional<Mat4> handAttachment(const Mat4& body) const;
    /** The costume's first posed node, native player.node->child; body when unbound. */
    Mat4 rootAttachment(const Mat4& body) const;
    std::optional<Mat4> attachment(const Mat4& body, std::string_view objectSuffix) const;
    bool heldWeaponBound() const { return m_handNode >= 0 && m_weapon.bound(); }
    s32 familiarTier() const { return m_familiar.tier(); }
    bool phoenixActive() const { return m_companion.kind() == PowerupCompanion::Kind::Phoenix; }
    /** Phoenix uses the opposite perch when an earned familiar occupies the native one.
     * Both its model and projectile use this same, body-scaled attachment. */
    Mat4 phoenixAttachment(const Mat4& body) const {
        return glm::translate(body, Vec3{m_phoenixSide, 0, 0});
    }
    const PowerupCompanion& companion() const { return m_companion; }
    /** Slot zero is earned, slot one is a timed powerup; both can be visible. */
    std::array<std::optional<CompanionVisual>, 2> companionVisuals(const Mat4& body,
                                                                   f32 alpha) const;
    bool familiarReleased() const { return m_familiarReleased; }
    const TreeModel& familiarMissile() const { return m_familiarMissile; }
    const std::filesystem::path& directory() const { return m_directory; }
    void setStrafe(StrafeWay way) { m_animator.setStrafe(way); }
    void setMelee(const MeleeSense& sense) { m_animator.setMelee(sense); }
    void setAttackSpeed(bool rapid, bool speed) { m_animator.setAttackSpeed(rapid, speed); }
    void setGuardArmor(f32 armor) { m_animator.setGuardArmor(armor); }
    void setShielded(bool shielded) { m_animator.setShielded(shielded); }
    void setPushed(bool pushed) { m_animator.setPushed(pushed); }
    /** Whose combo the body is held or thrown in, and whether the dwarf's ride goes on. */
    void setCombo(s32 grabberClass, bool ride) { m_animator.setCombo(grabberClass, ride); }
    /** Leaves the swing's ghosts of the weapon behind the body placed at `body` (the slow
     * swing, the spin and the middle power swing: action.c 816, 951, 976). */
    void updateTrail(const Mat4& body, s32 ticks);
    const WeaponTrail& trail() const { return m_trail; }
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
    /** Drawing-only matrices; never replace the gameplay hand/attachment transforms. */
    void preparePresentation(f32 frameBlend) const;
    std::optional<Mat4> visualAttachment(const Mat4& body, s32 node) const;
    std::optional<Mat4> visualAttachment(const Mat4& body, std::string_view suffix) const;
    /** An unlockable class without its own archive borrows the class it shadows. */
    static std::filesystem::path classFolder(const std::filesystem::path& root, s32 character,
                                             std::string_view sub);
    static std::string_view actionsClassOf(const std::filesystem::path& root, s32 character);
    void loadWeapon(const CharacterSave& save, RenderDevice& device);
    void loadMissile(const std::filesystem::path& root, const CharacterSave& save,
                     RenderDevice& device);
    void loadActions(const std::filesystem::path& root, const CharacterSave& save, bool enter);
    /** Binds an owned archive's model, loading shared texture pixels only when used. */
    bool bindModel(TreeModel& model, const TreeInfo& tree, ItemArchive& archive,
                   RenderDevice& device);
    void applyCostumeTextures(TreeModel& model, f32 frameBlend) const;
    /** Binds `object` of `archive` as the lone node of `tree` into `model` when it changed. */
    static void bindObject(RenderDevice& device, ItemArchive& archive, std::string_view object,
                           TreeInfo& tree, TreeModel& model);

    TextureSet m_sharedTextures;   ///< POWERUPS textures outlive every model borrowing them
    TextureSet m_familiarTextures; ///< WEAPONS owns the class SFX archives' animated frames
    std::filesystem::path m_sharedTextureDirectory;
    ItemArchive m_costumeArchive;
    TextureAnimator m_costumeTextures;
    AnimationSet m_actions;
    const TreeInfo* m_actionTree = nullptr;
    mutable TreeModel m_model;
    PlayerAnimator m_animator;
    u64 m_animationRevision = 0;
    const TreeInfo* m_costume = nullptr;
    std::filesystem::path m_directory;
    TreeInfo m_weaponTree;
    mutable TreeModel m_weapon;
    TreeInfo m_handItemTree;
    TreeModel m_handItem;
    bool m_handItemHeld = false;
    TreeInfo m_headwearTree;
    TreeModel m_headwear;
    TreeInfo m_gemTree;
    TreeModel m_gem;
    TreeInfo m_markerTree;
    TreeModel m_marker;
    TreeInfo m_armTree;
    TreeModel m_arm;
    s32 m_armNode = -1;   ///< the second hand, found as the costume loads
    s32 m_armHidden = -1; ///< what a borne shield hides: the class's own shield, or the hand
    bool m_armHeld = false;
    BlobShadow m_shadow;
    s32 m_handNode = -1;
    std::vector<s32> m_classNodeOfNode;
    std::vector<Mat4> m_transforms;
    mutable TreePose m_visualPose;
    mutable std::vector<Mat4> m_visualTransforms;
    ItemArchive m_effects;
    PlayerFamiliar m_familiar;
    PowerupCompanion m_companion;
    WeaponTrail m_trail;
    f32 m_companionAlpha = 1;
    s32 m_backNode = -1; ///< what wings hang from: the root's first child's first child
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
    f32 m_phoenixSide = 0;
};

} // namespace gdl::game
