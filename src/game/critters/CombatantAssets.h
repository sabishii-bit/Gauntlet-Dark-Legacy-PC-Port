#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"

#include "game/critters/CombatantDefinition.h"
#include "game/critters/CritterData.h"
#include "game/effects/BlobShadow.h"

namespace gdl::game {
/** Stable shared assets for a fighter family. Release only after all borrowers finish. */
struct CombatantAssets {
    CombatantDefinition definition;
    char realm = 0;                      ///< the realm whose costume the archive holds
    bool cooldownStartsWithMove = false; ///< decoded rules; ordinary GDL waits for its move
    f32 projectileLifetime = 0; ///< decoded flight lifetime; zero preserves ordinary GDL cues
    f32 wakeDelay = 2;          ///< minimum boss arming time; ordinary GDL uses two seconds
    bool initialMoveBeforeWake = false; ///< decoded INIT pose precedes the waking entrance
    CritterData data;
    std::vector<CritterData> children;
    ItemArchive archive;
    std::map<std::string, f32, std::less<>> effectLifetimes; ///< own and shared effect sequences
    const TreeInfo* tree = nullptr;
    TreeModel body;
    struct Attachment {
        CombatantAttachment definition;
        const TreeInfo* tree = nullptr;
        TreeModel model;
    };
    std::vector<Attachment> attachments;
    std::map<std::string, TreeModel> brokenModels; ///< PREFIX + D + node name replacements
    std::map<std::pair<std::string, std::string>, TreeModel> replacementModels;
    TreeModel* prepareReplacement(RenderDevice& device, std::string_view node,
                                  std::string_view object);
    TextureAnimator textures;
    std::map<std::string, std::vector<const Texture*>, std::less<>> skins;
    BlobShadow shadow; ///< SHADOW1L1 of its archive, when its type lies one
    /** The GMETER bar over the body, when its type hangs one (CritterAddHealthMeter). */
    const TreeInfo* meterTree = nullptr;
    TreeModel meter;
    std::unique_ptr<ItemArchive>
        meterArchive;   ///< shared meter artwork for families without their own
    s32 meterFill = -1; ///< RED_FILLE, stretched across by the health left
    CombatantAssets() = default;
    CombatantAssets(const CombatantAssets&) = delete;
    CombatantAssets& operator=(const CombatantAssets&) = delete;
    CombatantAssets(CombatantAssets&&) = delete;
    CombatantAssets& operator=(CombatantAssets&&) = delete;
    ~CombatantAssets();
    bool load(RenderDevice& device, const std::filesystem::path& root,
              const CombatantDefinition& family, char realm,
              std::span<TextureSet* const> textureLenders = {});
    /** Bind a validated platform decoder's own archive and move data, without
     * reading or replacing them with GameCube combatant records. */
    bool bindDecoded(RenderDevice& device, const std::filesystem::path& commonRoot,
                     const CombatantDefinition& family, char realm, ItemArchive source,
                     CritterData moves, std::vector<CritterData> childTypes,
                     std::span<TextureSet* const> textureLenders = {},
                     const AnimationSet* sharedEffects = nullptr);
    void clear();

private:
    bool bindPresentation(RenderDevice& device, const std::filesystem::path& commonRoot,
                          std::span<TextureSet* const> textureLenders,
                          const AnimationSet* sharedEffects);
    friend class Combatant;
    friend class Critters;
    friend class Bosses;
    friend class CombatantCheckpoint;
    friend class CombatantProjectilesCheckpoint;
    /** Reload/clear invalidates prepared borrowers even if allocator addresses repeat. */
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    /** Only weakly borrowed by local states; diagnostics must not keep it alive. */
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
