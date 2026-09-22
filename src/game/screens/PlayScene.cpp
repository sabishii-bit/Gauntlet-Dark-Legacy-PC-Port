#include "game/screens/PlayScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <format>
#include <numbers>

#include "engine/audio/AdsStream.h"
#include "engine/core/Log.h"
#include "engine/world/WorldCamera.h"

#include "game/players/ItemPickup.h"
#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kPlayersDirectory = "PLAYERS";
constexpr std::string_view kBeamObject = "L1XPLIGHTRAY01"; ///< the light on Sumner's lectern
constexpr std::string_view kWeaponsArchive = "WEAPONS";
constexpr std::string_view kSpawnEffect = "STARTFX"; ///< the tree the party materialises in
/** The stained-glass light through the window over the door: the Desecrated Temple's, lit once
 * its shards are all found. */
constexpr std::array<std::string_view, 2> kTempleLights{"L1XPLOWERLIGHTR", "L1XPUPPERLIGHTR"};
constexpr f32 kCutBarTop = 48.0f / 384.0f;    ///< the cut's black bars, as the original's trigger
constexpr f32 kCutBarBottom = 80.0f / 384.0f; ///< cameras draw them: shares of the height
constexpr std::string_view kNeedCrystals = "NEEDCRYSTALS";
constexpr std::string_view kNeedIcons = "NEEDGARGITEMS";
constexpr std::string_view kUnlockLevel = "UNLOCKLEVEL";
constexpr std::string_view kUnlockSection = "UNLOCKSECTION";
constexpr s32 kIconTierBase = 101;      ///< a gargoyle gate's trigger id less this is its tier
constexpr s32 kTitleY = 48;              ///< where the level's title starts, on the canvas
constexpr s32 kTitleLift = 16;           ///< how far up it slides, per unit of slide
constexpr f32 kTitleSlideStart = 0.025f; ///< the original's slide, growing by this a tick
constexpr f32 kTitleSlideRate = 0.025f;
constexpr f32 kTitleSlideEnd = 2.0f;     ///< where it stops, and sits once the camera rides
constexpr s32 kLevelsPerTier = 10;                          ///< costume tiers
constexpr s32 kWeaponTierTwoLevel = 10;                     ///< the weapon's tiers
constexpr s32 kWeaponTierThreeLevel = 50;
/** The costume objects the weapon hangs from, by class: the base classes, then the others. */
constexpr std::array<std::string_view, 3> kHandObjects{"R_WRIST", "RIGHTHAN", "RHEND"};
constexpr std::string_view kHeldWeapon = "WEAP_HOLD"; ///< a tiered costume's own weapon
constexpr std::string_view kClassAnimations = "ANIM"; ///< under the class folder
constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kStreamsDirectory = "STREAMS"; ///< the music, among the game's files
constexpr std::string_view kSoundDirectory = "audio";
constexpr std::string_view kAmbientBank = "TOWAMB"; ///< the tower's ambience, with its own bank
constexpr std::string_view kCommonBank = "COMMON";
constexpr std::array<std::string_view, 2> kStepSounds{"S_STEPROCK1", "S_STEPROCK2"};
constexpr std::string_view kPickupSound = "S_PICKUPMAGIC";
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kFontFile = "fonts/font32.json";
constexpr s32 kFont32SpaceWidth = 16;
constexpr std::string_view kFontTexture = "FONT32";
constexpr std::string_view kGlowTexture = "FONT32_GLOW";
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kButtonTexture = "BUTTON_TRI";
constexpr std::string_view kFireRingTexture = "GREENCIRCTRANS";
constexpr std::string_view kFireMaskTexture = "GREENCIRCTRANSM";
constexpr std::string_view kScrollTextFile = "text/scroll_e.json";
constexpr std::string_view kWelcomeMessage = "WELCOMEMESSAGE";
constexpr std::string_view kScrollBurnSound = "S_OPTMENUSCROLL"; ///< the options menu's, too
/** What a target sounds while it opens before the party and once it has, by the sound slot its
 * trigger names: the force fields and magic crossings, the lifts, the east gates, the west
 * gates. The tower's ambience bank keeps them under the audio directory's elevator slot
 * names; its own sample names call them ffield, lwrtwr, eastgat and westgat. */
struct OpeningSounds {
    std::string_view moving;
    std::string_view done;
};
constexpr std::array<OpeningSounds, 4> kOpeningSounds{{{"S_ELVMETL", "S_ELVMETSTPL"},
                                                       {"S_ELVROPEL", "S_ELVROPESTPL"},
                                                       {"S_ELVCHAINL", "S_ELVCHAINSTPL"},
                                                       {"S_ELVSTONEL", "S_ELVSTONESTPL"}}};

/** The sounds a trigger's slot names, or null for a slot without any. */
const OpeningSounds* openingSoundsOf(s32 slot) {
    return slot >= 0 && static_cast<usize>(slot) < kOpeningSounds.size()
               ? &kOpeningSounds[static_cast<usize>(slot)]
               : nullptr;
}
constexpr std::string_view kPromptText = "scroll.pressButton";
constexpr std::string_view kHintTextFile = "text/hints_e.json";
/** A potion by its kind (0 and 1 red, 2 blue, 3 yellow, 4 green): the bottle as it flies, the
 * magic it bursts into (fire, lightning, light, acid) and the sound of it. */
struct PotionLook {
    std::string_view bottle;
    std::string_view burst;
    std::string_view sound;
};
constexpr std::array<PotionLook, 5> kPotions{{{"POT_RED_TW", "MP_FIRE", "S_POTION2"},
                                              {"POT_RED_TW", "MP_FIRE", "S_POTION2"},
                                              {"POT_BLU_TW", "MP_ELEC", "S_POTION1"},
                                              {"POT_YEL_TW", "MP_LIGHT", "S_POTION3"},
                                              {"POT_GRE_TW", "MP_ACID", "S_POTION4"}}};
constexpr f32 kPotionToss = 5.0f;       ///< how hard a potion is thrown
constexpr f32 kBurstPerPower = 0.03125f; ///< a burst's size for each point of magic power
constexpr f32 kThrownShare = 0.75f;      ///< of that power a thrown potion keeps
constexpr f32 kPotionLoft = 0.707f;     ///< as much up as forwards
constexpr f32 kPotionHandHeight = 4.0f; ///< over the feet, where it leaves
constexpr f32 kPotionHandReach = 2.0f;  ///< and ahead of them
constexpr s32 kOgre = 12;
constexpr f32 kOgreScale = 1.6f;
constexpr f32 kMasterScale = 1.2f; ///< at level 99
constexpr std::string_view kSelectorMoveSound = "S_OPTMENUMOVHRZ";
constexpr std::string_view kChestSound = "S_CHEST";
constexpr std::string_view kNarratorBank = "VOICE1";
constexpr std::string_view kNarratorSecondBank = "VOICE2"; ///< the legend items' names
constexpr std::string_view kFirstRuneVoice = "S_RUNEFOUND1";
constexpr std::string_view kRuneVoicePrefix = "S_RUNE"; ///< then S_RUNE2 to S_RUNE12
constexpr std::string_view kLevelScrollPrefix = "SCROLLS"; ///< a level's scroll pages
constexpr std::string_view kWizardTree = "WIZARD";      ///< the good wizard, in a boss level's items
constexpr std::string_view kBossKeyTree = "BOSSKEY";    ///< the key that rises where the boss fell
constexpr std::string_view kBossKeyLaterTree = "BOSSKEY2";
constexpr f32 kBossKeySeconds = 30.0f;
constexpr std::string_view kBossKeySoundPrefix = "S_BOSSKEY"; ///< then the level's letter
constexpr f32 kWizardLift = 3.0f;   ///< how high over the party's middle the wizard hangs
constexpr f32 kCaptionScale = 0.75f;
constexpr s32 kCaptionBottom = 96;  ///< the captions sit this far up from the screen's foot
constexpr s32 kCaptionLineHeight = 18;
constexpr std::string_view kHelpTextPrefix = "help";
constexpr std::string_view kNoEffectTree = "NULLFX"; ///< a move's effect row that shows nothing
constexpr f32 kMoveNamedFrame = 1.0f; ///< of a turbo attack, when its name is announced
constexpr std::array<std::string_view, 5> kShieldTrees{"MS_FIRE", "MS_FIRE", "MS_ELEC", "MS_LIGHT",
                                                       "MS_ACID"};
constexpr std::array<std::string_view, 5> kShieldSounds{"S_SHIELD2", "S_SHIELD2", "S_SHIELD1",
                                                        "S_SHIELD3", "S_SHIELD4"};
constexpr f32 kShieldSeconds = 3.0f;      ///< how long a potion's ring lasts
constexpr f32 kShieldRadius = 25.0f;      ///< at full size; it is sized by the magic, as a burst is
constexpr f32 kShieldPotency = 0.25f;     ///< of the character's magic power, its harm
constexpr f32 kShieldHarmEvery = 0.5f;    ///< seconds between its harming what it touches
constexpr f32 kLevelUpEffectSeconds = 3.0f;   ///< the fanfare's ring about the character
constexpr f32 kKnockdownFrom = 1.0f;      ///< a blast must do more than this to floor anyone
constexpr f32 kBehind = 1.5707964f;       ///< a blow from further round than this is from behind
constexpr f32 kStrongThrowScale = 2.0f;  ///< a strong throw's weapon: twice the size and the harm
constexpr std::string_view kBlockEffect = "BLOCKFX";
constexpr f32 kBlockWorth = 2.0f;        ///< what a guard must take off a hurt for it to show
constexpr f32 kBlockPerDamage = 0.01f;   ///< seconds it shows for each point left
constexpr f32 kBlockLeast = 0.333f;
constexpr f32 kBlockMost = 1.0f;
constexpr f32 kChargeStick = 0.25f;  ///< the least push of the stick that steers a charge
constexpr f32 kRamDamage = 3.0f;     ///< what a charge does to what it runs into
constexpr f32 kRamReach = 0.3f;      ///< how near counts as run into
constexpr s32 kSpecialPowerup = 9;     ///< the pickup subtype of the specials
constexpr u32 kTurboFlag = 0x80000;    ///< of them, the one that fills the turbo meter
constexpr std::string_view kScrollTextPrefix = "scroll";
constexpr std::string_view kStringsFile = "text/english.json";
constexpr std::string_view kDeathSound = "S_PLAYERDIES";
constexpr std::string_view kWoodHitSound = "S_WEAPONHITWOOD";
constexpr std::string_view kBarrelBreakSound = "S_BARREL_WOOD"; ///< with the realm's letter
constexpr std::string_view kBarrelBlastSound = "S_BARREL_EXPLO";
constexpr std::string_view kBarrelGasSound = "S_BARREL_GAS";
constexpr std::string_view kFireTrapSound = "S_FIREHOLE";
constexpr std::string_view kBarrelBlast = "EXPLOSION";
constexpr std::string_view kBarrelGas = "POISONEXP1";
constexpr std::string_view kBarrelSmoke = "DESTSMOKE";
constexpr f32 kBlastRadius = 12.0f;      ///< of a chest or a barrel blowing up
constexpr f32 kChestBlastDamage = 50.0f; ///< each times the level's trap damage
constexpr f32 kBarrelBlastDamage = 30.0f;
constexpr f32 kGasDamage = 10.0f;
constexpr f32 kGasRadius = 6.5f;
constexpr f32 kGasSeconds = 4.0f;
constexpr f32 kGasGapSeconds = 0.5f;
constexpr f32 kPainEvery = 30.0f;        ///< harm from blows between cries
constexpr s32 kHeavyBlow = 60;           ///< a blow taking more than this is cried over at once
constexpr u32 kPainCries = 4;            ///< S_<CLS>PAIN1 to 4
constexpr std::string_view kHitSound = "S_PLYRDMG"; ///< a blow landing, now and then
constexpr s32 kHitSoundGapTicks = 30;
constexpr s32 kHealthLowMark = 150;      ///< down to here: "needs food, badly"
constexpr s32 kHealthLastMark = 50;      ///< and here: the life force, or about to die
constexpr std::string_view kBadlyLine = "S_BADLY";
constexpr std::string_view kLifeForceLine = "S_LIFEFORCE";
constexpr std::string_view kAboutToDieLine = "S_ABOUT";
constexpr f32 kFallenSeconds = 3.0f;     ///< from the last death to the tower
constexpr s32 kFireTrap = 1;
const Vec3 kNowhere{0.0f, -1.0e6f, 0.0f};
constexpr std::string_view kChestBlast = "EXPCHEST"; ///< a trapped chest going up

const PotionLook& potionLook(s32 kind) {
    return kPotions[kind >= 0 && static_cast<usize>(kind) < kPotions.size()
                        ? static_cast<usize>(kind)
                        : 0];
}
constexpr std::string_view kArrowTree = "ICON_ARROW";
constexpr std::string_view kMenuMoveSound = "S_OPTMENUMOVVRT";
constexpr std::string_view kMenuSelectSound = "S_OPTMENUSEL";
constexpr std::string_view kMenuExitSound = "S_OPTMENUEXIT";
/** Sumner's topics: the text id of each and the code the original's menu gives it. */
struct HintTopicEntry {
    std::string_view text;
    s32 code;
    HintTopic topic;
};
constexpr std::array<HintTopicEntry, 4> kHintTopics{{{"hints.general", 39, HintTopic::General},
                                                     {"hints.guardians", 40, HintTopic::Guardians},
                                                     {"hints.legends", 41, HintTopic::Legends},
                                                     {"hints.runestones", 42, HintTopic::Runestones}}};
constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr s32 kMinTicks = 1; ///< a frame advances the clock by at least one tick
constexpr s32 kMaxTicks = 4; ///< and, however late, by at most four

} // namespace

bool PlayScene::open(RenderDevice& device, const GameContext& context, LevelWorld& world,
                      std::span<const PartyMember> party, const PlayOptions& options) {
    close();
    m_context = context;
    m_device = &device;
    m_world = &world;
    if (!world.built() && !world.load(device, context.unpackedRoot)) {
        return false;
    }
    if (!m_boxes.load(device, context.unpackedRoot, context.strings)) {
        return false;
    }
    m_classes.load(context.unpackedRoot / kClassDataDirectory);
    loadSounds();
    loadIntroArt(device);
    // Sumner, his hints and his welcome belong to the tower alone.
    if (world.isTower()) {
        loadHintArt(device);
        m_sumner.load(device, world.items(), world.layout());
    }
    if (context.levels != nullptr) {
        m_portals.bind(device, world.layout(), world.items(), *context.levels,
                       &world.collision());
    }
    m_chests.bind(device, world.layout(), world.items(), &world.collision());
    m_gates.bind(device, world.layout(), world.items(), &world.collision());
    {
        const LevelInfo* level = world.level();
        const f32 gain = context.config != nullptr ? context.config->difficulty.gain() : 1.0f;
        m_traps.bind(device, world.layout(), world.items(), &world.collision(), 1,
                     level != nullptr ? level->tuning.trapTimeScale(gain) : 1.0f,
                     trapDamageScale());
    }
    m_barrels.bind(device, world.layout(), world.items(), &world.collision());
    m_strings.load(context.unpackedRoot / kStringsFile);
    // What the player reads is the string table's, so that it can be in any language.
    if (context.strings != nullptr) {
        m_strings.translate(*context.strings, kHelpTextPrefix);
    }
    m_help.clear();
    m_help.setTexts(&m_strings);
    m_refusedPortal = -1;
    m_leaving = false;
    m_transition.load(device, context.unpackedRoot);
    if (options.arriving) {
        m_transition.cover();
        m_transition.clearAway();
    }
    // Sumner's beam waits unseen until the party comes to him; the temple's light waits for
    // shards the save does not keep yet.
    m_beam = -1;
    m_beamAlpha = 0.0f;
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        const std::string& name = world.layout().objects()[i].name;
        if (name == kBeamObject) {
            m_beam = static_cast<s32>(i);
            world.setObjectAlpha(i, 0.0f);
        } else if (std::ranges::find(kTempleLights, name) != kTempleLights.end()) {
            world.setObjectAlpha(i, 0.0f);
        }
    }
    spawnParty(party, options);
    for (const DroppedItem& item : options.items) {
        world.placeItem(device, item.name, item.position);
    }
    world.setPlayerCount(static_cast<s32>(m_actors.size()));
    m_chests.setPlayerCount(static_cast<s32>(m_actors.size()));
    m_gates.setPlayerCount(static_cast<s32>(m_actors.size()));
    m_traps.setPlayerCount(static_cast<s32>(m_actors.size()));
    m_barrels.setPlayerCount(static_cast<s32>(m_actors.size()));
    bindEnemies(device, world, context);
    // The levels the party comes in at: what is gained from here is news.
    m_levels.clear();
    for (const PlayerActor& actor : m_actors) {
        m_levels.observe(actor.player(), experienceLevel(actor.save().experience()));
    }
    world.startTriggers(visitors());
    const std::array<SoundSet*, 2> banks{&m_ambientBank, &m_levelBank};
    m_ambience.bind(world.layout(), banks);
    for (const PlayerActor& actor : m_actors) {
        m_figures.push_back(loadFigure(device, actor.save()));
        m_subjects.push_back(CameraSubject{actor.position(), actor.followPoint()});
    }
    m_camera.reset(m_subjects, world.cameraMarkers(), world.cameraRange(), cameraView());
    if (bossCameraOn()) {
        m_bossCamera.reset(bossSubject(), m_subjects, *world.level()->bossCamera, cameraView());
    }
    startMusic();
    m_intro = Intro::None;
    // The party materialises first; Sumner's welcome, when it is due, follows.
    m_welcomePending = world.isTower() && options.welcome.value_or(freshParty(party));
    if (m_welcomePending) {
        m_world->hideCrystals(); // Sumner reveals them once the scroll has gone
    }
    // The start camera holds at the level's entrance and rides in to a party that stands
    // there; one back among a realm's portals (as when it has fallen, or come out of a level)
    // materialises with the follow camera already on it.
    const bool atEntrance =
        world.arrivalPoint(options.arrivalWorld) == world.startPoint(0);
    beginSpawn(device, !options.position.has_value() && atEntrance);
    loadPotionModels(device);
    m_open = true;
    log::info("Tower: {} in the party", m_actors.size());
    log::info("Level {} ({}): {} exit portals", world.ref().name, world.ref().title,
              m_portals.size());
    return true;
}

void PlayScene::close() {
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    stopVoice();
    stopOpeningSounds();
    m_scroll.close();
    m_hintMenu.close();
    m_hintMenu.setArt(HintMenuArt{});
    m_hintArrow = ModelSprite{};
    m_hintPlayer = -1;
    m_greetingLeft = -1.0f;
    m_hintsGiven = false;
    if (m_world != nullptr) {
        m_world->setPlayerCount(0);
    }
    m_sumner.clear();
    m_portals.clear();
    m_chests.clear();
    m_gates.clear();
    m_traps.clear();
    m_transition.release();
    m_leaving = false;
    m_missiles.clear(); // before the figures whose models they fly
    m_critterEffects.clear();
    m_effects.clear();  // and before the archive whose trees they play
    m_generators.clear();
    m_enemyMissiles.clear(); // before the archives whose trees they fly
    m_critters.close();
    m_bossMeter.clear(); // before the archive whose textures it draws
    m_victory.clear();
    m_wizardModel.clear();
    m_wizardTree = nullptr;
    m_sparkled = false;
    m_bosses.close();
    m_enemies.close();
    for (TreeModel& bottle : m_potionModels) {
        bottle.clear();
    }
    for (PowerupSelector& selector : m_selectors) {
        selector.close();
    }
    m_glowSheet = nullptr;
    m_playSeconds = 0.0f;
    m_text.setFont(nullptr, nullptr);
    m_staticTextures.releaseTextures();
    m_intro = Intro::None;
    m_actors.clear();
    m_slots.clear();
    m_down.clear();
    m_entrySaves.clear();
    m_painOwed.clear();
    m_hitSoundGaps.clear();
    m_struck.clear();
    m_turbo.clear();
    m_helpHeard.clear();
    m_moves.clear();
    m_rammed.clear();
    m_strikes.clear();
    m_strikeEffects.clear();
    m_strikeSources.clear();
    m_blockLeft.clear();
    m_shields.clear();
    m_dimmer.reset();
    if (m_world != nullptr) {
        m_world->setAmbientOffset(0.0f);
    }
    m_clouds.clear();
    m_blasts.clear();
    m_cloudGaps.clear();
    m_fallenSeconds = 0.0f;
    m_barrels.clear();
    m_help.clear();
    m_figures.clear();
    m_subjects.clear();
    if (m_context.sounds != nullptr) {
        m_ambience.stop(*m_context.sounds);
    }
    m_ambience.clear();
    m_spawns.clear();
    m_spawnTexmods.clear();
    m_weapons.release(); // its textures must go before the device does
    m_spawnTicks = 0;
    m_startCamera.stop();
    m_titleSlide = 0.0f;
    m_welcomePending = false;
    m_pickups.clear();
    m_boxes.release();
    m_world = nullptr;
    m_device = nullptr;
    m_open = false;
}

/** Stands the party side by side at the entrance, facing into the tower: the start marker's
 * heading points back out of the door. */
void PlayScene::spawnParty(std::span<const PartyMember> party, const PlayOptions& options) {
    // Arriving from a realm the party stands where the level marks that realm's way in.
    const WorldLocator* start = m_world->arrivalPoint(options.arrivalWorld);
    Vec3 origin{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    if (start != nullptr) {
        origin = start->position;
        yaw = start->rotation.y + kPi;
    } else if (!options.position.has_value()) {
        log::warn("Tower: no entrance start point; the party stands at the origin");
    }
    origin = options.position.value_or(origin);
    yaw = options.yaw.value_or(yaw);
    const Vec3 sideways{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const f32 first = -0.5f * static_cast<f32>(party.size() - 1) * kSpawnSpacing;
    for (usize i = 0; i < party.size(); ++i) {
        const PartyMember& member = party[i];
        PlayerActor actor;
        const Vec3 position = origin + sideways * (first + static_cast<f32>(i) * kSpawnSpacing);
        actor.spawn(member.player, member.save, m_classes.stats(member.save.character), position,
                    yaw);
        actor.settle(m_world->collision());
        m_actors.push_back(std::move(actor));
        m_slots.push_back(member.slot);
        // Someone who fell stands again in the tower; elsewhere they wait there still.
        m_down.push_back(member.fallen && !m_world->isTower() ? kInTower : kUp);
        m_entrySaves.push_back(member.save);
        m_painOwed.push_back(0.0f);
        m_hitSoundGaps.push_back(0);
        m_struck.push_back(PlayerDeed::None);
        m_turbo.emplace_back();
        m_turbo.back().add(member.turbo);
        m_helpHeard.push_back(member.helpHeard);
        std::ranges::sort(m_helpHeard.back());
        m_moves.emplace_back();
        m_rammed.emplace_back();
        m_blockLeft.push_back(0.0f);
        m_cloudGaps.push_back(0.0f);
    }
}

std::filesystem::path PlayScene::costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                   const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path base =
        unpackedRoot / kPlayersDirectory / std::string(cls) / std::string(costume);
    const s32 tier = experienceLevel(save.progress().experience) / kLevelsPerTier;
    const std::filesystem::path tiered =
        base.parent_path() / std::format("{}{}0", costume, tier);
    return std::filesystem::exists(tiered / "objects.json") ? tiered : base;
}

std::unique_ptr<PlayScene::Figure> PlayScene::loadFigure(RenderDevice& device,
                                                           const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path directory = costumeDirectory(m_context.unpackedRoot, save);
    auto figure = std::make_unique<Figure>();
    if (!figure->models.load(directory) || !figure->textures.load(directory) ||
        !figure->trees.load(directory)) {
        log::warn("Tower: no model for the {} {} under {}", costume, cls, directory.string());
        return nullptr;
    }
    const auto tree = figure->trees.find(std::format("{}_{}", cls, costume));
    if (!tree.has_value() ||
        !figure->model.bind(figure->trees.tree(*tree), figure->models, figure->textures, device)) {
        log::warn("Tower: the {} {} figure could not be built", costume, cls);
        return nullptr;
    }
    figure->costume = &figure->trees.tree(*tree);
    figure->directory = directory;
    loadWeapon(*figure, save, device);
    loadMissile(*figure, save, device);
    loadActions(*figure, save);
    return figure;
}

/** Finds the weapon as it flies (the costume's own throw tree, or the tier of the costume
 * colour's effects archive that the character's level earns) and the class's throw sound. */
void PlayScene::loadMissile(Figure& figure, const CharacterSave& save, RenderDevice& device) {
    const s32 level = experienceLevel(save.progress().experience);
    bool inCostume = true;
    const std::string name = MissileSpec::treeName(save.character, level, &inCostume);
    bool bound = false;
    if (inCostume) {
        if (const auto tree = figure.trees.find(name); tree.has_value()) {
            bound = figure.missile.bind(figure.trees.tree(*tree), figure.models, figure.textures,
                                        device);
        }
    } else if (figure.effects.load(
                   classFolder(save.character, std::format("SFX{}", colorCode(save.color))))) {
        if (const auto tree = figure.effects.trees.find(name); tree.has_value()) {
            bound = figure.missile.bind(figure.effects.trees.tree(*tree), figure.effects.models,
                                        figure.effects.textures, device);
        }
    }
    if (!bound) {
        log::warn("Tower: no {} for the {} to throw", name, classCode(save.character));
    }
    // The unlockable classes speak with the voice of the class they shadow.
    const std::string_view voice = classCode(save.character % kStartingClassCount);
    if (figure.voice.load(m_context.unpackedRoot / kSoundDirectory / voice)) {
        figure.throwSound = figure.voice.find(std::format("S_{}THROW", voice));
    }
}

/** Lets the weapon go: from the body's centre, out by the class's hand and a little ahead,
 * along the facing, as fast as the character's strength (or magic) throws. */
void PlayScene::throwWeapon(const PlayerActor& actor, Figure& /*figure*/) {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (&m_actors[i] == &actor) {
            launchWeapon(i, actor.facing(), 1.0f, true);
        }
    }
}

/** Sends the character's weapon off along `direction`, `scale` times its size and its harm;
 * with `spreads`, as many of them as a three or five way shot worn makes it. */
void PlayScene::launchWeapon(usize index, const Vec3& direction, f32 scale, bool spreads) {
    if (index >= m_actors.size() || m_figures[index] == nullptr) {
        return;
    }
    const PlayerActor& actor = m_actors[index];
    Figure& figure = *m_figures[index];
    const Vec3 facing = direction;
    const CharacterSave& save = actor.save();
    const ClassStats* stats = m_classes.stats(save.character);
    s32 stat = 0;
    Vec3 hand{0.0f, 0.0f, 0.0f};
    if (stats != nullptr) {
        const StatBlock block =
            displayStats(*stats, experienceLevel(save.experience()), save.progress());
        stat = MissileSpec::byMagic(save.character) ? block.magic() : block.strength();
        hand = stats->weaponOffset;
    }
    const Vec3 side{facing.z, 0.0f, -facing.x};
    MissileLaunch launch;
    launch.owner = actor.player();
    launch.scale = scale;
    launch.direction = facing;
    launch.position = actor.followPoint() + side * hand.x + Vec3{0.0f, hand.y, 0.0f} +
                      facing * (hand.z + PlayerMissiles::kMuzzle);
    launch.speed = PlayerMissiles::speedFor(stat);
    launch.damage = PlayerMissiles::damageFor(stat) * scale;
    launch.reach = PlayerMissiles::reachFor(figure.animator.attackSeconds());
    launch.spec = &MissileSpec::of(save.character);
    launch.model = &figure.missile;
    // Thrown into a wall at arm's length, nothing flies.
    const f32 radius = launch.spec->radius;
    const Vec3 clear = m_world->collision().resolveWalls(
        launch.position, radius, launch.position.y - radius * 0.5f,
        launch.position.y + radius * 0.5f);
    if (glm::distance(clear, launch.position) > 1e-4f) {
        return;
    }
    // A three or five way shot worn spreads the throw, fifteen degrees apart.
    const s32 shots = spreads ? PowerupEffects::of(save.progress().inventory).shots() : 1;
    for (const Vec3& way : PlayerMissiles::spread(facing, shots)) {
        launch.direction = way;
        m_missiles.launch(launch);
    }
    if (m_context.sounds != nullptr && figure.throwSound.has_value()) {
        try {
            m_context.sounds->play(figure.voice.sequence(*figure.throwSound), 1.0f,
                                   SoundCategory::Effects);
        } catch (const std::exception& e) {
            log::warn("Tower: throw sound: {}", e.what());
        }
    }
}

/** Hangs the weapon from the costume's hand: a tiered costume holds its own `WEAP_HOLD`,
 * an untiered one `WEAP_<colour>_HD<tier>` for the character's level, and the hand is the
 * object named after the class's wrist. */
void PlayScene::loadWeapon(Figure& figure, const CharacterSave& save, RenderDevice& device) {
    const s32 level = experienceLevel(save.progress().experience);
    s32 tier = 1;
    if (level >= kWeaponTierThreeLevel) {
        tier = 3;
    } else if (level >= kWeaponTierTwoLevel) {
        tier = 2;
    }
    std::string weapon{kHeldWeapon};
    if (!figure.models.find(weapon).has_value()) {
        weapon = std::format("WEAP_{}_HD{}", colorCode(save.color), tier);
    }
    if (!figure.models.find(weapon).has_value()) {
        log::warn("Tower: no {} in {}", weapon, figure.directory.string());
        return;
    }
    for (usize n = 0; n < figure.costume->nodes.size() && figure.handNode < 0; ++n) {
        for (const std::string_view suffix : kHandObjects) {
            if (figure.costume->nodes[n].object.ends_with(suffix)) {
                figure.handNode = static_cast<s32>(n);
                break;
            }
        }
    }
    if (figure.handNode < 0) {
        log::warn("Tower: no hand to hold {} in {}", weapon, figure.directory.string());
        return;
    }
    figure.weaponTree.name = weapon;
    TreeNodeInfo held;
    held.name = weapon;
    held.object = weapon;
    figure.weaponTree.nodes.push_back(held);
    if (!figure.weapon.bind(figure.weaponTree, figure.models, figure.textures, device)) {
        figure.handNode = -1;
    }
}

std::optional<std::filesystem::path> PlayScene::figureDirectory(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player && m_figures[i] != nullptr) {
            return m_figures[i]->directory;
        }
    }
    return std::nullopt;
}

bool PlayScene::weaponHeld(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player && m_figures[i] != nullptr) {
            // A move may empty the hand for a while.
            const bool hidden = i < m_moves.size() && m_moves[i].weaponHidden;
            return m_figures[i]->handNode >= 0 && m_figures[i]->weapon.bound() && !hidden;
        }
    }
    return false;
}

/** Sumner's beam comes up over three seconds while a player is near him and goes again once
 * they leave. */
void PlayScene::updateBeam(s32 ticks) {
    if (m_beam < 0) {
        return;
    }
    bool near = false;
    for (const PlayerActor& actor : m_actors) {
        near = near || glm::distance(actor.position(), m_sumner.position()) <= kBeamRadius;
    }
    const f32 step = static_cast<f32>(ticks) / static_cast<f32>(kBeamFadeTicks);
    const f32 alpha = std::clamp(m_beamAlpha + (near ? step : -step), 0.0f, 1.0f);
    if (alpha != m_beamAlpha) {
        m_beamAlpha = alpha;
        m_world->setObjectAlpha(static_cast<usize>(m_beam), alpha);
    }
}

/** Binds the class's sequences to the figure: the class tree carries the keys and its nodes
 * share their names with the costume's, so each costume node follows its namesake. */
std::filesystem::path PlayScene::classFolder(s32 character, std::string_view sub) const {
    const std::filesystem::path players = m_context.unpackedRoot / kPlayersDirectory;
    std::filesystem::path own = players / classCode(character) / sub;
    if (std::filesystem::exists(own)) {
        return own;
    }
    return players / classCode(character % kStartingClassCount) / sub;
}

std::string_view PlayScene::actionsClassOf(s32 character) const {
    const std::filesystem::path own =
        m_context.unpackedRoot / kPlayersDirectory / classCode(character) / kClassAnimations;
    return classCode(std::filesystem::exists(own) ? character
                                                  : character % kStartingClassCount);
}

void PlayScene::loadActions(Figure& figure, const CharacterSave& save) {
    const std::string_view cls = actionsClassOf(save.character);
    const std::filesystem::path directory = classFolder(save.character, kClassAnimations);
    const auto tree = figure.actions.load(directory) ? figure.actions.find(cls) : std::nullopt;
    if (!tree.has_value() || !figure.animator.bind(figure.actions.tree(*tree))) {
        log::warn("Tower: no sequences for the {} under {}; the figure stands still", cls,
                  directory.string());
        return;
    }
    const TreeInfo& actions = figure.actions.tree(*tree);
    figure.classNodeOfNode.clear();
    for (const TreeNodeInfo& node : figure.costume->nodes) {
        const auto match = actions.findNode(node.name);
        figure.classNodeOfNode.push_back(match.has_value() ? static_cast<s32>(*match) : -1);
    }
    figure.animate(0.0f, 0, 0.0f);
}

/** Finds the footstep sounds in the common bank. */
void PlayScene::loadSounds() {
    m_stepSounds.fill(std::nullopt);
    if (const LevelAudioInfo* audio = m_world->audio(); audio != nullptr) {
        m_levelBank.load(m_context.unpackedRoot / kSoundDirectory / audio->bank);
    }
    m_ambientBank.load(m_context.unpackedRoot / kSoundDirectory / kAmbientBank);
    m_narrator.load(m_context.unpackedRoot / kSoundDirectory / kNarratorBank);
    m_narratorSecond.load(m_context.unpackedRoot / kSoundDirectory / kNarratorSecondBank);
    if (!m_commonSounds.load(m_context.unpackedRoot / kSoundDirectory / kCommonBank)) {
        return;
    }
    for (usize foot = 0; foot < kStepSounds.size(); ++foot) {
        m_stepSounds[foot] = m_commonSounds.find(kStepSounds[foot]);
    }
    m_pickupSound = m_commonSounds.find(kPickupSound);
}

void PlayScene::playCommon(std::optional<u32> sound) {
    if (m_context.sounds == nullptr || !sound.has_value()) {
        return;
    }
    m_context.sounds->play(m_commonSounds.sequence(*sound), 1.0f, SoundCategory::Effects);
}

/** Takes what the party stands on: a crystal counts for everyone, towards its realm's gate,
 * up to what the gate wants; the taker's box gets the card, every box the count. */
void PlayScene::collectItems() {
    if (m_device == nullptr) {
        return;
    }
    std::vector<Collector> collectors;
    collectors.reserve(m_actors.size());
    for (const PlayerActor& actor : m_actors) {
        // Against an open chest, a character reaches what lies in it.
        const Vec3 here = presenceOf(collectors.size());
        const s32 chest = m_chests.holdingTouchedBy(ChestVisitor{here, actor.radius()});
        const Vec3 from =
            chest >= 0 ? m_chests.chest(static_cast<usize>(chest)).figure.position() : here;
        collectors.push_back(Collector{from, actor.reach(), actor.height() * 0.5f});
    }
    const std::vector<Pickup> pickups = m_world->collect(
        *m_device, collectors, [this](const Pickup& pickup) { return takePickup(pickup); });
    for (usize chest = 0; chest < m_chests.size(); ++chest) {
        const s32 held = m_chests.chest(chest).held;
        if (held >= 0 && m_world->placedItems().item(static_cast<usize>(held)).taken) {
            m_chests.remove(chest);
        }
    }
    for (const Pickup& pickup : pickups) {
        if (pickup.realm <= 0) {
            continue; // handed over as it was judged
        }
        if (pickup.realm > 0 && static_cast<usize>(pickup.realm) < kRealmCount) {
            const s32 wanted = LevelTriggers::crystalsNeeded(pickup.realm);
            bool enough = wanted > 0;
            for (PlayerActor& actor : m_actors) {
                s32& count = actor.save().progress().crystals[static_cast<usize>(pickup.realm)];
                if (wanted <= 0 || count < wanted) {
                    ++count;
                }
                enough = enough && count >= wanted;
                m_pickups.showCount(actor.player(), PickupHud::crystalIcon(pickup.realm), count,
                                    wanted);
            }
            if (pickup.collector < m_actors.size()) {
                m_pickups.addCard(m_actors[pickup.collector].player(), PickupHud::kCrystalCard);
            }
            if (enough) {
                announceUnlock(pickup.realm);
            }
        }
        playCommon(m_pickupSound);
    }
}

/** The level's chests, gates and traps under the party: nobody walks through a chest or a
 * gate that is shut; against one, a key carried is spent and it opens (a chest's sound is
 * the common one, a gate's its realm's); an opened chest drops what it held, pays its gold
 * to its opener or blows up; a trap that is out hurts whoever is in it. */
void PlayScene::updateFixtures(s32 ticks, f32 seconds) {
    std::vector<Obstacle> boxes = m_chests.obstacles();
    const std::vector<Obstacle> barred = m_gates.obstacles();
    boxes.insert(boxes.end(), barred.begin(), barred.end());
    const std::vector<Obstacle> casks = m_barrels.obstacles();
    boxes.insert(boxes.end(), casks.begin(), casks.end());
    m_barrels.update(seconds);
    std::vector<ChestVisitor> visitors;
    std::vector<TrapVictim> victims;
    visitors.reserve(m_actors.size());
    victims.reserve(m_actors.size());
    for (usize i = 0; i < m_actors.size(); ++i) {
        PlayerActor& actor = m_actors[i];
        if (isDown(i)) {
            visitors.push_back(ChestVisitor{kNowhere, actor.radius(), 0});
            victims.push_back(TrapVictim{kNowhere, actor.radius()});
            continue;
        }
        Vec3 position = actor.position();
        for (const Obstacle& box : boxes) {
            position = box.pushOut(position, actor.radius());
        }
        actor.place(position);
        visitors.push_back(ChestVisitor{position, actor.radius(),
                                        actor.save().progress().inventory.keys});
        victims.push_back(TrapVictim{position, actor.radius()});
    }
    for (const ChestEvent& event : m_chests.update(seconds, visitors)) {
        if (event.visitor >= m_actors.size()) {
            continue;
        }
        PlayerActor& actor = m_actors[event.visitor];
        switch (event.kind) {
        case ChestEvent::Kind::Unlocked:
            if (m_chests.chest(event.chest).locked) {
                actor.save().progress().inventory.spendKey();
            }
            playNamed(kChestSound);
            break;
        case ChestEvent::Kind::Opened:
            if (event.explodes) {
                if (m_device != nullptr && m_weapons.loaded()) {
                    m_effects.start(*m_device, m_weapons, kChestBlast, event.position);
                }
                playRealmSound(kBarrelBlastSound);
                m_chests.remove(event.chest);
                postHelp(HelpMessages::kChestsExplode, event.visitor);
                blast(event.position, kBlastRadius, kChestBlastDamage * trapDamageScale());
            } else if (event.gold > 0) {
                takeItem(actor.save(), ItemOffer{static_cast<s32>(ItemKind::Gold), event.gold});
                m_pickups.addCard(actor.player(), "GOLD");
                playNamed(kPickupSound);
            } else if (event.contents >= 0 && m_device != nullptr) {
                // It lies in the open chest, for whoever touches the chest next.
                const s32 count = m_chests.chest(event.chest).count;
                if (m_world->placeItemRecord(*m_device, event.contents, event.position, count)) {
                    m_chests.hold(event.chest,
                                  static_cast<s32>(m_world->placedItems().size()) - 1);
                }
            } else {
                m_chests.remove(event.chest);
            }
            break;
        case ChestEvent::Kind::Refused:
            postHelp(HelpMessages::kChestNeedsKey, event.visitor);
            break;
        }
    }
    for (const GateEvent& event : m_gates.update(ticks, seconds, visitors)) {
        if (event.visitor >= m_actors.size()) {
            continue;
        }
        if (event.kind == GateEvent::Kind::Unlocked) {
            m_actors[event.visitor].save().progress().inventory.spendKey();
            playGateSound(0);
        } else if (event.kind == GateEvent::Kind::Refused) {
            postHelp(HelpMessages::kDoorNeedsKey, event.visitor);
        }
    }
    for (const TrapHit& hit : m_traps.update(ticks, seconds, victims)) {
        if (hit.victim >= m_actors.size()) {
            continue;
        }
        if (hit.subtype == kFireTrap) {
            playRealmSound(kFireTrapSound);
        }
        // Every trap stuns: spikes and blades make their victim flinch, the rest reel.
        if (guarded(hit.victim, hit.damage, false) > 1.0f && !isDown(hit.victim)) {
            m_struck[hit.victim] = hit.pierces ? PlayerDeed::Flinch : PlayerDeed::Reel;
        }
        hurt(hit.victim, hit.damage, hit.pierces ? HurtKind::Pierce : HurtKind::Burn);
        postHelp(HelpMessages::kTrapsHurt, hit.victim);
    }
    updateClouds(seconds);
    for (s32& gap : m_hitSoundGaps) {
        gap = std::max(gap - ticks, 0);
    }
}

/** What the level's traps and blasts are scaled by: its own trap damage and the
 * difficulty's gain. */
f32 PlayScene::trapDamageScale() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    const f32 gain =
        m_context.config != nullptr ? m_context.config->difficulty.gain() : 1.0f;
    return level != nullptr ? level->tuning.trapDamageScale(gain) : gain;
}

Vec3 PlayScene::presenceOf(usize index) const {
    return index < m_actors.size() && !isDown(index) ? m_actors[index].position() : kNowhere;
}

bool PlayScene::fallen(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player) {
            return isDown(i);
        }
    }
    return false;
}

void PlayScene::hurtPlayer(s32 player, f32 damage, HurtKind kind, bool directed) {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player) {
            hurt(i, damage, kind, directed);
        }
    }
}

/** What a guard or a shove leaves of a hurt over a point, by the original's rules as it
 * shipped: a raised guard halves what comes from somewhere and takes all of what comes from
 * nowhere in particular (a trap underfoot); a shove halves either. */
f32 PlayScene::guarded(usize index, f32 damage, bool directed) const {
    const Figure* figure = index < m_figures.size() ? m_figures[index].get() : nullptr;
    if (figure == nullptr || damage <= 1.0f) {
        return damage;
    }
    if (figure->animator.defending()) {
        return directed ? damage * 0.5f : 0.0f;
    }
    return figure->animator.shoving() ? damage * 0.5f : damage;
}

/** A sound of the realm's bank, whose names end in the realm's letter. */
SoundHandle PlayScene::playRealmSound(std::string_view stem) {
    const std::string& level = m_world->ref().name;
    const char letter = level.empty() ? 'G' : level.front();
    return playNamed(std::format("{}{}", stem, letter));
}

const TurboMeter* PlayScene::turboMeter(s32 player) const {
    for (usize i = 0; i < m_actors.size() && i < m_turbo.size(); ++i) {
        if (m_actors[i].player() == player) {
            return &m_turbo[i];
        }
    }
    return nullptr;
}

/** What the turbo and charge buttons ask, by the original's rules: turbo held as the attack
 * button goes down, the greater turbo attack with the meter full, the lesser with two fifths
 * of it, else nothing (the attack is then an ordinary one); the charge button going down, a
 * shove, which wants a twentieth. Nothing when the body is in the middle of something. */
PlayerDeed PlayScene::turboDeed(usize index, const PlayInput& in) const {
    if (index >= m_turbo.size() || m_figures[index] == nullptr) {
        return PlayerDeed::None;
    }
    const TurboMeter& meter = m_turbo[index];
    PlayerDeed deed = PlayerDeed::None;
    if (in.turbo && in.attackPressed) {
        if (meter.held() >= TurboMeter::kFullCost) {
            deed = PlayerDeed::TurboFull;
        } else if (meter.held() >= TurboMeter::kStrongCost) {
            deed = PlayerDeed::TurboStrong;
        }
    } else if (in.chargePressed && meter.held() >= TurboMeter::kShoveFrom) {
        deed = PlayerDeed::Shove;
    }
    return m_figures[index]->animator.canBegin(deed) ? deed : PlayerDeed::None;
}

/** A charge goes flat out the way the stick is pushed, or straight ahead when it is not. */
MoveInput PlayScene::chargeInput(usize index, const MoveInput& stick, f32 cameraYaw) const {
    MoveInput rush;
    rush.magnitude = 1.0f;
    if (stick.magnitude >= kChargeStick) {
        rush.direction = stick.direction;
        return rush;
    }
    const Vec3 facing = m_actors[index].facing();
    const f32 ahead = std::atan2(facing.x, facing.z) - cameraYaw;
    rush.direction = Vec2{std::sin(ahead), std::cos(ahead)};
    return rush;
}

/** What a charge runs into is struck, once each charge. */
void PlayScene::ramBarrels(usize index) {
    const PlayerActor& actor = m_actors[index];
    std::vector<usize>& rammed = m_rammed[index];
    for (usize barrel = 0; barrel < m_barrels.size(); ++barrel) {
        if (!m_barrels.standing(barrel) || std::ranges::find(rammed, barrel) != rammed.end() ||
            !m_barrels.barrel(barrel).box.touchedBy(actor.position(), actor.radius(), kRamReach)) {
            continue;
        }
        rammed.push_back(barrel);
        strikeBarrel(barrel, kRamDamage, actor.player());
    }
    settleBlasts();
}

/** A turbo attack begins: the strikes its class's data gives it are lined up, and what it
 * costs is owed until the first of them that does harm is made. A class whose data has none
 * pays at once and cries out, so the move is never free. */
void PlayScene::beginMove(usize index) {
    MoveProgress& move = m_moves[index];
    move.pending.clear();
    move.all.clear();
    move.owed = 0.0f;
    move.named = false;
    const PlayerAnimator::Action action = m_figures[index]->animator.action();
    const bool full = action == PlayerAnimator::Action::TurboFull;
    move.weaponHidden = false;
    move.volleysShot.clear();
    const ClassStats* known = m_classes.stats(m_actors[index].save().character);
    if (action == PlayerAnimator::Action::StrongThrow) {
        // The strong throw costs nothing; its strikes are only what it shows and sounds.
        if (known != nullptr) {
            move.pending = known->strikesOf(known->moves.turboAThrow);
            move.all = move.pending;
            move.volleysShot.assign(move.all.size(), 0);
        }
        return;
    }
    if (!full && action != PlayerAnimator::Action::TurboStrong) {
        return;
    }
    move.owed = full ? TurboMeter::kFullCost : TurboMeter::kStrongCost;
    if (const ClassStats* stats = known) {
        if (full) {
            move.pending = stats->strikesOf(stats->moves.turboC1);
            const std::vector<s32> second = stats->strikesOf(stats->moves.turboC2);
            move.pending.insert(move.pending.end(), second.begin(), second.end());
        } else {
            move.pending = stats->strikesOf(stats->moves.turboB);
        }
    }
    move.all = move.pending;
    move.volleysShot.assign(move.all.size(), 0);
    if (move.pending.empty()) {
        m_turbo[index].spend(move.owed);
        move.owed = 0.0f;
        cry(index, full ? "TURBOC" : "TURBOB");
    }
}

/** Makes the strikes of the move under way whose frames have come; a move cut short makes
 * no more of them, and what it still owed is never paid. */
void PlayScene::runMove(usize index) {
    MoveProgress& move = m_moves[index];
    const PlayerAnimator& body = m_figures[index]->animator;
    const bool attacking = body.action() == PlayerAnimator::Action::TurboFull ||
                           body.action() == PlayerAnimator::Action::TurboStrong ||
                           body.action() == PlayerAnimator::Action::StrongThrow;
    if (!attacking) {
        move.pending.clear();
        move.all.clear();
        move.owed = 0.0f;
        move.weaponHidden = false;
        return;
    }
    const ClassStats* stats = m_classes.stats(m_actors[index].save().character);
    if (stats == nullptr) {
        return;
    }
    const f32 frame = body.player().frame();
    // A frame in, the move is named.
    if (!move.named && frame >= kMoveNamedFrame) {
        move.named = true;
        for (const s32 strike : move.all) {
            if (const s32 help = stats->moveStrikes[static_cast<usize>(strike)].help; help >= 0) {
                postHelp(help, index);
                break;
            }
        }
    }
    // The level goes dark for as long as a strike that darkens it lasts; the hand is empty
    // for as long as one that hides the weapon does; a volley lets fly as it lasts.
    move.weaponHidden = false;
    for (usize slot = 0; slot < move.all.size(); ++slot) {
        const MoveStrike& row = stats->moveStrikes[static_cast<usize>(move.all[slot])];
        if (!row.lasting(frame)) {
            continue;
        }
        if (row.dimming() != 0.0f) {
            m_dimmer.ask(row.dimming());
        }
        if ((row.flags & MoveStrike::kHidesWeapon) != 0) {
            move.weaponHidden = true;
        }
        if (row.type == MoveStrike::kVolley) {
            runVolley(index, slot, row, frame);
        }
    }
    std::vector<s32> due;
    std::erase_if(move.pending, [&](s32 strike) {
        const bool now =
            frame >= static_cast<f32>(stats->moveStrikes[static_cast<usize>(strike)].startFrame);
        if (now) {
            due.push_back(strike);
        }
        return now;
    });
    for (const s32 strike : due) {
        fireStrike(index, strike);
    }
}

/** The costume colour's effects, which hold the trees a class's moves show; loaded when
 * first wanted. */
ItemArchive* PlayScene::moveEffectsOf(usize index) {
    Figure* figure = index < m_figures.size() ? m_figures[index].get() : nullptr;
    if (figure == nullptr) {
        return nullptr;
    }
    if (!figure->effects.loaded()) {
        // An unlockable class has none of its own: its moves show the trees of the class it
        // shadows.
        const CharacterSave& save = m_actors[index].save();
        figure->effects.load(
            classFolder(save.character, std::format("SFX{}", colorCode(save.color))));
    }
    return figure->effects.loaded() ? &figure->effects : nullptr;
}

/** What a character's own blows do, which a strike with a negative amount multiplies. */
f32 PlayScene::ownDamageOf(usize index) const {
    const CharacterSave& save = m_actors[index].save();
    const ClassStats* stats = m_classes.stats(save.character);
    if (stats == nullptr) {
        return PlayerMissiles::kLeastDamage;
    }
    const StatBlock block =
        displayStats(*stats, experienceLevel(save.experience()), save.progress());
    return PlayerMissiles::damageFor(MissileSpec::byMagic(save.character) ? block.magic()
                                                                         : block.strength());
}

/** One strike of a move: its effects show and sound where the character stands, the meter
 * pays what the move still owes if the strike does harm, and the harm is set going. */
void PlayScene::fireStrike(usize index, s32 strikeIndex) {
    const ClassStats* stats = m_classes.stats(m_actors[index].save().character);
    if (stats == nullptr || strikeIndex < 0 ||
        static_cast<usize>(strikeIndex) >= stats->moveStrikes.size()) {
        return;
    }
    const MoveStrike& strike = stats->moveStrikes[static_cast<usize>(strikeIndex)];
    const PlayerActor& actor = m_actors[index];
    const Vec3 facing = actor.facing();
    MoveProgress& move = m_moves[index];
    if (strike.amount != 0.0f && move.owed > 0.0f) {
        m_turbo[index].spend(move.owed);
        move.owed = 0.0f;
    }
    // A span that only lasts, or a volley, harms nothing of itself; the rest are set going.
    u32 id = 0;
    if (strike.harms()) {
        id = m_strikes.start(strike, actor.player(), actor.position(), facing,
                             ownDamageOf(index));
        m_strikeSources.push_back(StrikeSource{id, index, strikeIndex});
    }
    const Vec3 origin = MoveStrikes::originOf(strike, actor.position(), facing);
    const MoveStrikes::Strike* started = m_strikes.find(id);
    ItemArchive* archive = moveEffectsOf(index);
    // An effect may bring another with it.
    usize followed = 0;
    for (s32 at = strike.effect; at >= 0 && static_cast<usize>(at) < stats->moveEffects.size() &&
                                 followed < stats->moveEffects.size();
         at = stats->moveEffects[static_cast<usize>(at)].next, ++followed) {
        const MoveEffect& effect = stats->moveEffects[static_cast<usize>(at)];
        if (!effect.sound.empty()) {
            if (const auto sound = m_figures[index]->voice.find(effect.sound);
                sound.has_value() && m_context.sounds != nullptr) {
                m_context.sounds->play(m_figures[index]->voice.sequence(*sound), 1.0f,
                                       SoundCategory::Effects);
            } else {
                playNamed(effect.sound);
            }
        }
        if (effect.tree.empty() || effect.tree == kNoEffectTree || archive == nullptr ||
            m_device == nullptr || !archive->trees.find(effect.tree).has_value()) {
            continue;
        }
        EffectTrees::Setting setting;
        setting.scale = effect.scale;
        setting.yaw = std::atan2(facing.x, facing.z);
        if (started != nullptr && started->flies) {
            setting.velocity = facing * started->speed;
            setting.seconds = started->secondsLeft;
            // What flies launches once, then its looping tree carries it on.
            if (at == strike.effect && strike.loopEffect >= 0 &&
                static_cast<usize>(strike.loopEffect) < stats->moveEffects.size()) {
                setting.then = stats->moveEffects[static_cast<usize>(strike.loopEffect)].tree;
            }
        }
        const Vec3 side{facing.z, 0.0f, -facing.x};
        const Vec3 at3 = origin + side * effect.offset.x + Vec3{0.0f, effect.offset.y, 0.0f} +
                         facing * effect.offset.z;
        const u32 shown = m_effects.startSet(*m_device, *archive, effect.tree, at3, setting);
        if (shown != 0 && started != nullptr && started->flies) {
            m_strikeEffects.push_back(StrikeEffect{id, shown});
        }
    }
}

/** The strikes under way harm what they reach: the barrels, for now. What flies takes its
 * effect along, and the effect ends with it. */
void PlayScene::updateStrikes(f32 seconds) {
    for (const StrikeHit& hit : m_strikes.update(seconds, &m_world->collision())) {
        const auto source = std::ranges::find(m_strikeSources, hit.strike, &StrikeSource::strike);
        // The swarm and the generators in its reach take it, with the row's damage type.
        u32 flags = 0;
        if (source != m_strikeSources.end() && source->actor < m_actors.size()) {
            const ClassStats* stats = m_classes.stats(m_actors[source->actor].save().character);
            if (stats != nullptr && source->row >= 0 &&
                static_cast<usize>(source->row) < stats->moveStrikes.size()) {
                flags = static_cast<u32>(stats->moveStrikes[static_cast<usize>(source->row)].damageType);
            }
        }
        for (const s32 enemy : m_enemies.reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            const Vec3 direction = m_enemies.positionOf(enemy) - hit.centre;
            strikeEnemy(enemy, hit.damage, flags, Vec3{direction.x, 0.0f, direction.z}, hit.owner);
        }
        for (const s32 generator : m_generators.within(hit.centre, hit.radius)) {
            strikeGenerator(generator, hit.damage, hit.owner);
        }
        if (m_bosses.reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            EnemyHit struck;
            struck.damage = hit.damage;
            struck.flags = flags;
            struck.player = hit.owner;
            struck.close = true;
            if (const Vec3* at = m_bosses.position(); at != nullptr) {
                struck.direction = Vec3{at->x - hit.centre.x, 0.0f, at->z - hit.centre.z};
                struck.where = hit.centre + glm::normalize(struck.direction) * hit.radius;
            }
            m_bosses.hurt(struck);
        }
        for (const s32 critter : m_critters.reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            const Vec3 direction = m_critters.positionOf(critter) - hit.centre;
            strikeCritter(critter, hit.damage, flags, Vec3{direction.x, 0.0f, direction.z}, hit.owner,
                          std::nullopt, true);
        }
        for (usize barrel = 0; barrel < m_barrels.size(); ++barrel) {
            if (!m_barrels.standing(barrel)) {
                continue;
            }
            const Breakables::Barrel& cask = m_barrels.barrel(barrel);
            if (!hit.reaches(cask.figure.position(), cask.radius, cask.height)) {
                continue;
            }
            strikeBarrel(barrel, hit.damage, hit.owner);
            // What it harms shows the strike's own mark, when its class gives it one.
            if (source == m_strikeSources.end() || source->actor >= m_actors.size()) {
                continue;
            }
            const ClassStats* stats = m_classes.stats(m_actors[source->actor].save().character);
            ItemArchive* archive = moveEffectsOf(source->actor);
            if (stats == nullptr || archive == nullptr || m_device == nullptr) {
                continue;
            }
            const s32 mark = stats->moveStrikes[static_cast<usize>(source->row)].hitEffect;
            if (mark >= 0 && static_cast<usize>(mark) < stats->moveEffects.size()) {
                const MoveEffect& effect = stats->moveEffects[static_cast<usize>(mark)];
                if (!effect.tree.empty() && archive->trees.find(effect.tree).has_value()) {
                    m_effects.start(*m_device, *archive, effect.tree, cask.figure.position(),
                                    effect.scale);
                }
                if (!effect.sound.empty()) {
                    playNamed(effect.sound);
                }
            }
        }
    }
    settleBlasts();
    std::erase_if(m_strikeSources, [this](const StrikeSource& source) {
        return m_strikes.find(source.strike) == nullptr;
    });
    std::erase_if(m_strikeEffects, [this](const StrikeEffect& pair) {
        if (m_strikes.find(pair.strike) != nullptr) {
            return false;
        }
        m_effects.stop(pair.effect);
        return true;
    });
}

/** Experience won, as the original awards it: scaled by the level (its own scale, less the
 * further the character is past the level the place is meant for); what a kill wins also
 * feeds the turbo meter, unless the character is in the middle of a turbo move. */
void PlayScene::harm(s32 player, f32 damage, HurtKind kind) {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player) {
            hurt(i, damage, kind);
        }
    }
}

void PlayScene::awardExperience(s32 player, s32 amount, bool kill) {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() != player || isDown(i) || amount <= 0) {
            continue;
        }
        CharacterSave& save = m_actors[i].save();
        const LevelInfo* level = m_world->level();
        const f32 scale = level != nullptr
                              ? level->tuning.experienceScale(experienceLevel(save.experience()))
                              : 1.0f;
        const auto won = static_cast<s32>(static_cast<f32>(amount) * scale);
        save.progress().experience += won;
        const bool busy = m_figures[i] != nullptr && m_figures[i]->animator.turboing();
        if (kill && !busy) {
            m_turbo[i].add(TurboMeter::kPerExperience * static_cast<f32>(won));
        }
    }
}

/** A volley lets one of the character's own missiles fly every so many frames while it
 * lasts, off the facing by its angle, which closes to nothing (or opens from it) across the
 * span when it sweeps. */
void PlayScene::runVolley(usize index, usize slot, const MoveStrike& strike, f32 frame) {
    MoveProgress& move = m_moves[index];
    if (slot >= move.volleysShot.size()) {
        return;
    }
    const f32 since = frame - static_cast<f32>(strike.startFrame);
    const f32 every = strike.delay > 0.0f ? strike.delay : 1.0e6f; // none: one shot only
    const auto due = static_cast<s32>(std::floor(since / every)) + 1;
    const f32 span = static_cast<f32>(strike.endFrame - strike.startFrame);
    while (move.volleysShot[slot] < due) {
        const f32 shotAt = static_cast<f32>(move.volleysShot[slot]) * every;
        f32 angle = strike.angle;
        if ((strike.flags & (MoveStrike::kSweepsIn | MoveStrike::kSweepsOut)) != 0) {
            f32 through = span > 0.0f ? std::clamp(shotAt / span, 0.0f, 1.0f) : 1.0f;
            if ((strike.flags & MoveStrike::kSweepsIn) != 0) {
                through = 1.0f - through;
            }
            angle *= through;
        }
        const Vec3 facing = m_actors[index].facing();
        const f32 heading = std::atan2(facing.x, facing.z) + angle;
        launchWeapon(index, Vec3{std::sin(heading), 0.0f, std::cos(heading)}, 1.0f, false);
        ++move.volleysShot[slot];
    }
}

/** Which way a step along `heading` is for a character facing `facing`: within an eighth of a
 * turn of ahead or behind it is that, else to the side it lies on. */
StrafeWay PlayScene::strafeWayOf(f32 heading, f32 facing) {
    constexpr f32 kEighth = 0.7853982f;
    const f32 off = std::remainder(heading - facing, 8.0f * kEighth);
    if (std::abs(off) > 3.0f * kEighth) {
        return StrafeWay::Back;
    }
    if (off > kEighth) {
        return StrafeWay::Right; // the original's sense of a positive turn
    }
    return off < -kEighth ? StrafeWay::Left : StrafeWay::Forward;
}

/** A potion spent on a shield: its magic rings the character for a few seconds, going about
 * with them, to the potion's shield sound. */
void PlayScene::shieldPotion(usize index) {
    PlayerActor& actor = m_actors[index];
    const s32 kind = actor.save().progress().inventory.takePotion();
    if (kind == 0) {
        return;
    }
    const auto look = static_cast<usize>(std::clamp(kind, 0, 4));
    const f32 power = magicPowerOf(actor);
    const f32 size = std::min(kBurstPerPower * power, 1.0f);
    PotionShield shield;
    shield.actor = index;
    shield.radius = kShieldRadius * size;
    shield.damage = kShieldPotency * power;
    shield.secondsLeft = kShieldSeconds;
    if (m_device != nullptr && m_weapons.loaded() &&
        m_weapons.trees.find(kShieldTrees[look]).has_value()) {
        EffectTrees::Setting setting;
        setting.scale = size;
        setting.seconds = kShieldSeconds;
        shield.effect = m_effects.startSet(*m_device, m_weapons, kShieldTrees[look],
                                           actor.position(), setting);
    }
    playNamed(kShieldSounds[look]);
    m_shields.push_back(shield);
}

/** The rings go about with their characters and harm the barrels they touch. */
void PlayScene::updateShields(f32 seconds) {
    for (PotionShield& shield : m_shields) {
        shield.secondsLeft -= seconds;
        if (shield.actor >= m_actors.size() || isDown(shield.actor)) {
            shield.secondsLeft = 0.0f;
            continue;
        }
        const Vec3 at = m_actors[shield.actor].position();
        m_effects.moveTo(shield.effect, at);
        shield.harmIn -= seconds;
        if (shield.harmIn > 0.0f) {
            continue;
        }
        shield.harmIn = kShieldHarmEvery;
        for (const usize barrel : m_barrels.within(at, shield.radius)) {
            strikeBarrel(barrel, shield.damage, m_actors[shield.actor].player());
        }
    }
    settleBlasts();
    std::erase_if(m_shields, [this](const PotionShield& shield) {
        if (shield.secondsLeft > 0.0f) {
            return false;
        }
        m_effects.stop(shield.effect);
        return true;
    });
}

/** A guard that took enough off a hurt shows it: the block effect about the character, for
 * longer the more got through, and not again until that is over. */
void PlayScene::showBlock(usize index, f32 taken, f32 left) {
    if (index >= m_blockLeft.size() || m_blockLeft[index] > 0.0f || taken <= kBlockWorth) {
        return;
    }
    const f32 shown = std::clamp(kBlockPerDamage * left, kBlockLeast, kBlockMost);
    m_blockLeft[index] = shown;
    if (m_device != nullptr && m_weapons.loaded() &&
        m_weapons.trees.find(kBlockEffect).has_value()) {
        EffectTrees::Setting setting;
        setting.seconds = shown;
        m_effects.startSet(*m_device, m_weapons, kBlockEffect, m_actors[index].followPoint(),
                           setting);
    }
}

/** Runs a character's meter: a turbo attack is paid for as it first does harm, a shove runs
 * it down while it lasts, and otherwise it climbs while the character is free to act, the
 * narrator saying so when it comes full. */
void PlayScene::updateTurbo(usize index, s32 ticks, f32 seconds) {
    if (index >= m_turbo.size() || m_figures[index] == nullptr) {
        return;
    }
    TurboMeter& meter = m_turbo[index];
    const PlayerAnimator& body = m_figures[index]->animator;
    if (body.turboBegan()) {
        beginMove(index);
    }
    runMove(index);
    if (body.action() == PlayerAnimator::Action::Shove) {
        meter.drain(seconds);
    } else if (!isDown(index) && !body.turboing() && meter.fill(seconds)) {
        postHelp(HelpMessages::kUseTurbo, index);
    }
    meter.step(ticks);
}

/** One of a character's own cries, `which` being what follows its class in the name. */
void PlayScene::cry(usize index, std::string_view which) {
    Figure* body = index < m_figures.size() ? m_figures[index].get() : nullptr;
    if (body == nullptr || m_context.sounds == nullptr) {
        return;
    }
    const std::string_view voice =
        classCode(m_actors[index].save().character % kStartingClassCount);
    if (const auto sound = body->voice.find(std::format("S_{}{}", voice, which));
        sound.has_value()) {
        m_context.sounds->play(body->voice.sequence(*sound), 1.0f, SoundCategory::Effects);
    }
}

/** Puts a help message up over a character, the narrator saying it, unless the party has
 * seen it. */
bool PlayScene::postHelp(s32 id, usize index, s32 number) {
    if (index >= m_actors.size()) {
        return false;
    }
    std::vector<HelpReader> readers;
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (!isDown(i)) {
            readers.push_back(HelpReader{m_actors[i].player(), &m_actors[i].save().helpSeen,
                                         i < m_helpHeard.size() ? &m_helpHeard[i] : nullptr});
        }
    }
    const HelpMessageSpec* spec = m_help.post(id, m_actors[index].player(), readers, number);
    if (spec == nullptr) {
        return false;
    }
    if (m_context.sounds != nullptr) {
        // A turbo attack's name is called from the character's own class's bank; the
        // narrator's lines are in either of its banks.
        std::vector<SoundSet*> banks{&m_narrator, &m_narratorSecond};
        if (spec->classVoice && index < m_figures.size() && m_figures[index] != nullptr) {
            banks = {&m_figures[index]->voice};
        }
        for (SoundSet* bank : banks) {
            if (const auto line = bank->find(spec->voice); line.has_value()) {
                m_context.sounds->play(bank->sequence(*line), 1.0f, SoundCategory::Effects);
                break;
            }
        }
    }
    return true;
}

/** A blow on a barrel: wood sounds under it until it breaks, when what it held is left
 * lying, or it blows up, or its gas hangs where it stood. */
void PlayScene::strikeBarrel(usize barrel, f32 power, s32 byPlayer) {
    const auto struck = m_barrels.strike(barrel, power);
    if (!struck.has_value()) {
        return;
    }
    if (!struck->broken) {
        playNamed(kWoodHitSound);
        return;
    }
    const auto effect = [&](std::string_view tree) {
        if (m_device != nullptr && m_weapons.loaded()) {
            m_effects.start(*m_device, m_weapons, tree, struck->position);
        }
    };
    switch (struck->kind) {
    case BreakableStrike::Kind::Plain:
    case BreakableStrike::Kind::Holding:
        playRealmSound(kBarrelBreakSound);
        effect(kBarrelSmoke);
        if (struck->contents >= 0 && m_device != nullptr &&
            m_world->placeItemRecord(*m_device, struck->contents, struck->position,
                                     struck->count)) {
            for (usize i = 0; i < m_actors.size(); ++i) {
                if (m_actors[i].player() == byPlayer) {
                    postHelp(HelpMessages::kBarrelsHold, i);
                }
            }
        }
        break;
    case BreakableStrike::Kind::Exploding:
        playRealmSound(kBarrelBlastSound);
        effect(kBarrelBlast);
        m_blasts.push_back(
            Blast{struck->position, kBlastRadius, kBarrelBlastDamage * trapDamageScale()});
        break;
    case BreakableStrike::Kind::Poison:
        playRealmSound(kBarrelGasSound);
        effect(kBarrelGas);
        m_clouds.push_back(GasCloud{struck->position, kGasDamage * trapDamageScale(),
                                    kGasSeconds});
        break;
    }
}

/** Whoever is within a blast is hurt by it, and the barrels within it are struck by it (so
 * one that blows up sets off its neighbours). */
void PlayScene::blast(const Vec3& position, f32 radius, f32 damage) {
    m_blasts.push_back(Blast{position, radius, damage});
    settleBlasts();
}

/** Feels out every blast waiting, and those they set off in turn. */
void PlayScene::settleBlasts() {
    while (!m_blasts.empty()) {
        const Blast felt = m_blasts.back();
        m_blasts.pop_back();
        for (usize i = 0; i < m_actors.size(); ++i) {
            if (isDown(i)) {
                continue;
            }
            const PlayerActor& actor = m_actors[i];
            const Vec3 offset = actor.followPoint() - felt.position;
            if (std::hypot(offset.x, offset.z) <= felt.radius + actor.radius() &&
                std::abs(offset.y) <= actor.height() * 0.5f + felt.radius) {
                // A blast that gets through knocks its victim off their feet: onto their face
                // when it came from behind them, onto their back otherwise.
                const bool guarding = m_figures[i] != nullptr && m_figures[i]->animator.guarding();
                if (guarded(i, felt.damage, true) > kKnockdownFrom && !guarding &&
                    !m_world->isTower()) {
                    const Vec3 push = actor.position() - felt.position;
                    f32 round = std::atan2(push.x, push.z) - actor.yaw();
                    round = std::remainder(round, 2.0f * kBehind * 2.0f);
                    m_struck[i] = std::abs(round) > kBehind ? PlayerDeed::FallBack
                                                            : PlayerDeed::FallForward;
                }
                hurt(i, felt.damage, HurtKind::Blow, true);
            }
        }
        for (const usize barrel : m_barrels.within(felt.position, felt.radius)) {
            strikeBarrel(barrel, felt.damage, -1);
        }
        for (const s32 enemy : m_enemies.within(felt.position, felt.radius)) {
            const Vec3 away = m_enemies.positionOf(enemy) - felt.position;
            strikeEnemy(enemy, felt.damage, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z}, -1);
        }
        for (const s32 generator : m_generators.within(felt.position, felt.radius)) {
            strikeGenerator(generator, felt.damage, -1);
        }
        if (m_bosses.within(felt.position, felt.radius)) {
            EnemyHit struck;
            struck.damage = felt.damage;
            struck.flags = EnemyHit::kKnockDown;
            if (const Vec3* at = m_bosses.position(); at != nullptr) {
                struck.direction = Vec3{at->x - felt.position.x, 0.0f, at->z - felt.position.z};
            }
            m_bosses.hurt(struck);
        }
        for (const s32 critter : m_critters.within(felt.position, felt.radius)) {
            const Vec3 away = m_critters.positionOf(critter) - felt.position;
            strikeCritter(critter, felt.damage, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z}, -1);
        }
    }
}

/** As the original's AddExp has it: a level gained posts "LEVEL n", plays the class's
 * fanfare about the character and heals a hundred; a tenth level besides changes the costume
 * and has the class say its piece (`S_EXP10WAR`, up to `S_EXP99`). */
void PlayScene::updateLevels() {
    for (usize i = 0; i < m_actors.size(); ++i) {
        CharacterSave& save = m_actors[i].save();
        const auto change = m_levels.observe(m_actors[i].player(), experienceLevel(save.experience()));
        if (!change.has_value() || !change->gained()) {
            continue;
        }
        postHelp(HelpMessages::kLevelUp, i, change->to);
        save.progress().health += static_cast<s32>(kLevelUpHealth);
        if (m_device != nullptr && m_weapons.loaded()) {
            const std::string tree = std::format("LEVELUP_{}", colorCode(save.color));
            if (m_weapons.trees.find(tree).has_value()) {
                EffectTrees::Setting setting;
                setting.seconds = kLevelUpEffectSeconds;
                const u32 effect = m_effects.startSet(*m_device, m_weapons, tree, m_actors[i].position(), setting);
                m_effects.moveTo(effect, m_actors[i].position());
            }
        }
        if (change->milestone() && m_device != nullptr) {
            const s32 tier = std::min(change->to / LevelChange::kLevelsPerTier, 9);
            const std::string_view cls = classCode(save.character);
            if (playNamed(std::format("S_EXP{}0{}", tier, cls.substr(0, 3))) == kNoSound) {
                playNamed("S_EXP99ALL");
            }
            // The costume of the new tier, weapon and all, where the character stands.
            if (auto figure = loadFigure(*m_device, save); figure != nullptr) {
                m_figures[i] = std::move(figure);
            }
        }
    }
}

/** A hit on one of the great ones. */
void PlayScene::strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                              std::optional<Vec3> where, bool close) {
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    hit.where = where;
    hit.close = close;
    for (const PlayerActor& actor : m_actors) {
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    m_critters.hurt(id, hit);
}

/** Plays what one of the great ones (the boss with `ofBoss`) has set off: its tree from its
 * own archive (or the weapons', which holds the common marks of a hit) where it happened,
 * riding along with it when it follows, and its sound. */
void PlayScene::showCritterCue(const CritterCue& cue, ItemArchive* archive, bool ofBoss) {
    if (archive == nullptr || !archive->trees.find(cue.tree).has_value()) {
        archive = m_weapons.loaded() && m_weapons.trees.find(cue.tree).has_value() ? &m_weapons : nullptr;
    }
    if (!cue.tree.empty() && archive != nullptr && m_device != nullptr) {
        EffectTrees::Setting setting;
        setting.scale = cue.scale;
        setting.yaw = cue.yaw;
        setting.seconds = cue.life;
        if (const u32 effect = m_effects.startSet(*m_device, *archive, cue.tree, cue.position, setting);
            effect != 0 && cue.follows) {
            const Vec3* at = ofBoss ? m_bosses.position() : &m_critters.positionOf(cue.critter);
            m_critterEffects.push_back(
                CritterEffect{effect, cue.critter, ofBoss, at != nullptr ? cue.position - *at : Vec3{0.0f, 0.0f, 0.0f}});
        }
    }
    if (!cue.sound.empty()) {
        playNamed(cue.sound);
    }
}

/** Effects riding on the great ones go where they go, and are let go of when they end. */
void PlayScene::followCritterEffects() {
    for (usize i = 0; i < m_critterEffects.size();) {
        const CritterEffect& riding = m_critterEffects[i];
        const bool alive = riding.ofBoss ? m_bosses.present() : m_critters.alive(riding.critter) ||
                                                                   m_critters.dying(riding.critter);
        if (!m_effects.playing(riding.effect) || !alive) {
            m_critterEffects.erase(m_critterEffects.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const Vec3* at = riding.ofBoss ? m_bosses.position() : &m_critters.positionOf(riding.critter);
        if (at != nullptr) {
            m_effects.moveTo(riding.effect, *at + riding.offset);
        }
        ++i;
    }
}

/** The boss has fallen: everyone in play gets its shard, its key rises where it fell, the
 * meter goes, and the wizard's visit is set going. */
void PlayScene::bossFallen(const Vec3& where) {
    const LevelInfo* level = m_world->level();
    if (level == nullptr || m_victory.running() || m_victory.finished()) {
        return;
    }
    const s32 order = LevelRef::orderOf(m_world->ref().realmId);
    u16 found = 0;
    for (PlayerActor& actor : m_actors) {
        actor.save().progress().relics.addShard(order);
        found |= actor.save().progress().relics.runes;
    }
    // The realm's runestones are those its levels' records number, from one.
    u16 inRealm = 0;
    if (m_context.levels != nullptr) {
        for (const s32 rune : m_context.levels->runesOf(m_world->ref().realm)) {
            if (rune > 0 && rune <= Relics::kRuneCount) {
                inRealm |= static_cast<u16>(1U << static_cast<u32>(rune - 1));
            }
        }
    }
    const char letter = m_world->ref().name.empty() ? 'G' : m_world->ref().name.front();
    m_victory.begin(level->bossType, letter, inRealm, found, false);
    if (m_device != nullptr && m_world->items().loaded()) {
        EffectTrees::Setting setting;
        setting.seconds = kBossKeySeconds;
        setting.then = kBossKeyLaterTree;
        m_effects.startSet(*m_device, m_world->items(), kBossKeyTree, where, setting);
        loadWizard(*m_device);
    }
    playNamed(std::format("{}{}", kBossKeySoundPrefix, letter));
}

/** The good wizard's figure from the level's own archive, stood over the party. */
void PlayScene::loadWizard(RenderDevice& device) {
    m_wizardTree = nullptr;
    ItemArchive& items = m_world->items();
    const auto tree = items.trees.find(kWizardTree);
    if (!tree.has_value()) {
        log::warn("Tower: no {} in the level's items; the wizard is not seen", kWizardTree);
        return;
    }
    const TreeInfo& figure = items.trees.tree(*tree);
    if (!m_wizardModel.bind(figure, items.models, items.textures, device)) {
        return;
    }
    m_wizardTree = &figure;
    if (!figure.sequences.empty()) {
        m_wizardPlayer.start(figure.sequences[0], 0);
    }
    m_wizardPose.rest(figure);
    // Over the middle of the boss's mark and the party, three units up, facing them.
    Vec3 centre = m_bosses.position() != nullptr ? *m_bosses.position() : Vec3{0.0f, 0.0f, 0.0f};
    f32 count = 1.0f;
    Vec3 party{0.0f, 0.0f, 0.0f};
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (!isDown(i)) {
            centre += m_actors[i].position();
            party += m_actors[i].position();
            count += 1.0f;
        }
    }
    m_wizardPosition = centre / count + Vec3{0.0f, kWizardLift, 0.0f};
    const Vec3 toParty = party / std::max(count - 1.0f, 1.0f) - m_wizardPosition;
    m_wizardYaw = std::atan2(toParty.x, toParty.z);
}

/** The wizard's visit runs on: he fades in, says his piece (typed out under the view, his
 * lines from the level's bank), then the party sparkles and is taken to the tower. */
void PlayScene::updateVictory(s32 ticks, f32 seconds) {
    if (!m_victory.running()) {
        return;
    }
    std::vector<usize> pageLengths;
    if (const auto& caption = m_victory.caption(); caption.has_value()) {
        if (const auto found = m_strings.find(caption->message); found.has_value()) {
            for (const std::string& page : m_strings.message(*found).pages) {
                pageLengths.push_back(page.size());
            }
        }
    }
    for (const VictoryVoice& voice : m_victory.update(ticks, pageLengths)) {
        playNamed(voice.sound);
    }
    if (m_wizardTree != nullptr && m_victory.wizardShown()) {
        m_wizardPlayer.advance(seconds, true);
        m_wizardPose.evaluate(*m_wizardTree, m_wizardPlayer.sequence(), m_wizardPlayer.frame());
    }
    if (m_victory.sparkling() && !m_sparkled) {
        m_sparkled = true;
        if (m_device != nullptr && m_weapons.loaded()) {
            for (usize i = 0; i < m_actors.size(); ++i) {
                if (!isDown(i)) {
                    m_effects.start(*m_device, m_weapons, kSpawnEffect, m_actors[i].position());
                }
            }
        }
    }
    if (m_victory.finished() && !m_leaving) {
        m_destination = LevelRef::tower();
        m_leaving = true;
        m_transition.comeUp();
    }
}

void PlayScene::drawWizard(RenderDevice& device, const Mat4& clip) const {
    if (m_wizardTree == nullptr || !m_victory.wizardShown() || m_victory.wizardAlpha() <= 0.0f) {
        return;
    }
    const Mat4 model = glm::rotate(glm::translate(Mat4{1.0f}, m_wizardPosition), m_wizardYaw,
                                   Vec3{0.0f, 1.0f, 0.0f});
    m_wizardModel.draw(device, clip, model, m_world->lighting(), m_wizardPose.matrices(), nullptr,
                       m_victory.wizardAlpha());
}

/** The wizard's words, typed out under the view. */
void PlayScene::drawCaption(f32 width, f32 height) {
    const auto& caption = m_victory.caption();
    if (!caption.has_value() || !m_text.ready()) {
        return;
    }
    const auto found = m_strings.find(caption->message);
    if (!found.has_value() || caption->page >= m_strings.message(*found).pages.size()) {
        return;
    }
    const std::string shown = m_strings.message(*found).pages[caption->page].substr(0, caption->shown);
    const std::vector<std::string> lines = ScrollBox::splitLines(shown);
    TextStyle style;
    style.scale = kCaptionScale;
    s32 y = static_cast<s32>(height) - kCaptionBottom - static_cast<s32>(lines.size()) * kCaptionLineHeight;
    for (const std::string& line : lines) {
        m_text.draw(m_canvas, -static_cast<s32>(width / 2.0f), y, line, style);
        y += kCaptionLineHeight;
    }
}

/** The boss's worth goes the great ones' way: shares to the hitter, a kill's to everyone. */
void PlayScene::awardBossLosses() {
    for (const CritterLoss& loss : m_bosses.takeLosses()) {
        if (loss.killed) {
            bossFallen(loss.position);
        }
        for (const PlayerActor& actor : m_actors) {
            const s32 player = actor.player();
            if ((loss.player >= 0 && loss.player != player) || player < 0 ||
                static_cast<usize>(player) >= m_critterExperienceOwed.size()) {
                continue;
            }
            f32& owed = m_critterExperienceOwed[static_cast<usize>(player)];
            owed += loss.experience;
            const auto whole = static_cast<s32>(std::floor(owed));
            if (whole > 0) {
                owed -= static_cast<f32>(whole);
                awardExperience(player, whole, loss.killed);
            }
        }
    }
}

/** What the great ones are worth: a share to whoever hurt one, whole points as they add
 * up, and a kill's share to everyone. */
void PlayScene::awardCritterLosses() {
    for (const CritterLoss& loss : m_critters.takeLosses()) {
        // A gargoyle slain leaves the key its form is named by where it fell.
        if (loss.killed && loss.kind == kGargoyleCritter && !loss.form.empty() && m_device != nullptr) {
            m_world->placeItem(*m_device, "GARG" + loss.form, loss.position);
        }
        for (const PlayerActor& actor : m_actors) {
            const s32 player = actor.player();
            if (loss.player >= 0 && loss.player != player) {
                continue;
            }
            if (player < 0 || static_cast<usize>(player) >= m_critterExperienceOwed.size()) {
                continue;
            }
            f32& owed = m_critterExperienceOwed[static_cast<usize>(player)];
            owed += loss.experience;
            const auto whole = static_cast<s32>(std::floor(owed));
            if (whole > 0) {
                owed -= static_cast<f32>(whole);
                awardExperience(player, whole, loss.killed);
            }
        }
    }
}

/** The level's swarm and the generators that breed it, at the level's scales: what they can
 * take and deal is the level's own, how fast they go and see and how a generator breeds grow
 * with the difficulty setting. Placements of ordinary strength stand where the level puts
 * them, asleep when of no strength; the archer, bomber and suicide variants wait for their
 * missiles. */
void PlayScene::bindEnemies(RenderDevice& device, LevelWorld& world, const GameContext& context) {
    const LevelInfo* level = world.level();
    const f32 gain = context.config != nullptr ? context.config->difficulty.gain() : 1.0f;
    EnemyScales scales;
    GeneratorScales breeding;
    s32 most = Enemies::kMost;
    if (level != nullptr) {
        scales.health = level->tuning.enemyHealth;
        scales.speed = level->tuning.enemySpeedScale(gain);
        scales.sight = level->tuning.enemySightScale(gain);
        scales.damage = level->tuning.enemyDamage;
        scales.playerLevel = level->tuning.playerLevel;
        breeding.health = level->tuning.generatorHealth;
        breeding.rate = level->tuning.generatorRateScale(gain);
        breeding.most = level->tuning.generatorMostScale(gain);
        most = level->maxEnemies;
    }
    const auto seed = static_cast<u32>(std::hash<std::string>{}(world.ref().name));
    m_enemies.open(device, context.unpackedRoot, &world.collision(), most, scales, seed);
    const std::string& levelName = world.ref().name;
    m_critters.open(device, context.unpackedRoot, &world.collision(), scales,
                    levelName.empty() ? 'G' : levelName.front());
    m_bosses.open(device, context.unpackedRoot, &world.collision(), scales,
                  levelName.empty() ? 'G' : levelName.front());
    m_critterExperienceOwed.fill(0.0f);
    const auto players = static_cast<s32>(m_actors.size());
    const std::span<const LevelEnemy> roster =
        level != nullptr ? std::span<const LevelEnemy>(level->enemies) : std::span<const LevelEnemy>{};
    m_generators.bind(device, world.layout(), m_enemies, &world.collision(), breeding, players,
                      roster);
    // The level's boss, at its boss mark.
    if (level != nullptr && !bossNameOf(level->bossType).empty()) {
        if (const WorldLocator* mark = world.layout().findLocator(LocatorKind::Boss); mark != nullptr) {
            m_bosses.spawn(level->bossType, mark->position, mark->rotation.y);
            if (const CritterMeter* meter = m_bosses.meter(); meter != nullptr) {
                ItemArchive* archive = m_bosses.archive();
                m_bossMeter.bind(*meter, archive != nullptr ? &archive->textures : nullptr);
                const BossView boss = m_bosses.view();
                m_bossMeter.update(0, boss.health, boss.maxHealth, boss.alive, false);
            }
            // The first of the party carrying its legend item brings it to the fight.
            for (const PlayerActor& actor : m_actors) {
                if (actor.save().progress().relics.hasLegend(m_bosses.legendRealm()) &&
                    m_bosses.bringLegend(actor.player())) {
                    break;
                }
            }
        }
    }
    const std::vector<ItemInfo>& infos = world.layout().itemInfos();
    for (const ItemInstance& instance : world.layout().itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kPlacedEnemy || !shownToParty(instance.minPlayers, players)) {
            continue;
        }
        const auto named = enemyKindOf(info.name);
        const s32 strength = Generators::paramOf(instance, 0);
        if (!named.has_value()) {
            continue;
        }
        const std::optional<s32> kind = levelKindOf(roster, *named, strength);
        // The great ones stand where they are put, facing as placed.
        const Mat4 stood = itemPlacement(instance.position, instance.rotation);
        const f32 facing = std::atan2(stood[2][0], stood[2][2]);
        if (*kind == kGolemEnemyKind) {
            m_critters.spawn(kGolemCritter, instance.position, facing);
            continue;
        }
        if (*kind == kGeneralEnemyKind) {
            m_critters.spawn(kGeneralCritter, instance.position, facing);
            continue;
        }
        if (*kind == kGargoyleEnemyKind) {
            m_critters.spawn(kGargoyleCritter, instance.position, facing);
            continue;
        }
        if (*kind >= kSwarmKindCount || !m_enemies.loadKind(*kind)) {
            continue;
        }
        EnemySpawn spawn;
        spawn.kind = *kind;
        spawn.tier = std::max(strength, 1);
        spawn.algorithm = Generators::paramOf(instance, 1);
        if (const s32 interval = Generators::paramOf(instance, 3); interval > 0) {
            spawn.idleTicks = interval;
        }
        spawn.position = instance.position;
        const Mat4 placement = itemPlacement(instance.position, instance.rotation);
        spawn.direction = Vec3{placement[2][0], 0.0f, placement[2][2]};
        spawn.placed = true;
        spawn.asleep = strength == 0;
        m_enemies.spawn(spawn, {}, m_generators.obstacles());
    }
}

/** The party as the swarm sees it: where each stands, how big, how seasoned; the fallen and
 * those in the tower are not seen. */
std::vector<EnemyView> PlayScene::enemyViews() const {
    std::vector<EnemyView> views;
    views.reserve(m_actors.size());
    for (usize i = 0; i < m_actors.size(); ++i) {
        const PlayerActor& actor = m_actors[i];
        EnemyView view;
        view.player = actor.player();
        view.position = actor.position();
        view.radius = actor.radius();
        view.height = actor.height();
        view.level = experienceLevel(actor.save().experience());
        view.hidden = isDown(i);
        views.push_back(view);
    }
    return views;
}

/** The generators breed, the swarm goes about its business, and what it lands on the party
 * is taken: a power blow from a tall one is a knock that makes its victim flinch. What the
 * party has done to it is paid in experience. */
void PlayScene::updateEnemies(s32 ticks, f32 seconds) {
    const std::vector<EnemyView> views = enemyViews();
    std::vector<Obstacle> boxes = m_generators.obstacles();
    const std::vector<Obstacle> chests = m_chests.obstacles();
    boxes.insert(boxes.end(), chests.begin(), chests.end());
    const std::vector<Obstacle> barred = m_gates.obstacles();
    boxes.insert(boxes.end(), barred.begin(), barred.end());
    const std::vector<Obstacle> casks = m_barrels.obstacles();
    boxes.insert(boxes.end(), casks.begin(), casks.end());
    const LevelInfo* level = m_world->level();
    const f32 missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1.0f;
    m_generators.update(ticks, m_enemies, views, boxes);
    m_enemies.update(ticks, seconds, views, boxes, &m_enemyMissiles, missileSpeed);
    m_enemyMissiles.update(seconds, &m_world->collision(), views);
    // What the throwers let fly lands on the party, or bursts where it fell; what blows
    // itself up blasts everything about it.
    for (const EnemyMissileHit& hit : m_enemyMissiles.takeHits()) {
        for (usize i = 0; i < m_actors.size(); ++i) {
            if (hit.player >= 0 && m_actors[i].player() == hit.player && !isDown(i)) {
                const bool guarding = m_figures[i] != nullptr && m_figures[i]->animator.guarding();
                if ((hit.flags & EnemyMissileKind::kKnockBack) != 0 && !guarding &&
                    m_struck[i] == PlayerDeed::None) {
                    m_struck[i] = PlayerDeed::Flinch;
                }
                hurt(i, hit.damage, HurtKind::Pierce, true);
            }
        }
        if (hit.burstRadius > 0.0f) {
            blast(hit.position, hit.burstRadius, hit.damage);
        }
    }
    for (const EnemyBurst& burst : m_enemies.takeBursts()) {
        blast(burst.position, kBlastRadius, burst.damage);
    }
    settleBlasts();
    m_critters.update(ticks, seconds, views);
    m_bosses.update(ticks, seconds, views);
    if (m_bossMeter.bound()) {
        const BossView boss = m_bosses.view();
        m_bossMeter.update(ticks, boss.health, boss.maxHealth, m_bosses.present() && boss.alive,
                           m_bosses.frozen());
    }
    // The legend item held up is the bearer's no more.
    for (const LegendEvent& event : m_bosses.takeLegendEvents()) {
        if (event.cue != LegendCue::Brandished) {
            continue;
        }
        for (PlayerActor& actor : m_actors) {
            if (actor.player() == event.player) {
                actor.save().progress().relics.spendLegend(event.realm);
            }
        }
    }
    for (const CritterBlow& blow : m_bosses.takeBlows()) {
        for (usize i = 0; i < m_actors.size(); ++i) {
            if (m_actors[i].player() == blow.player && !isDown(i)) {
                hurt(i, blow.damage, HurtKind::Blow, true);
            }
        }
    }
    awardBossLosses();
    updateVictory(ticks, seconds);
    // The great ones' effects and sounds: a move's, a strike's, a hit's.
    for (const CritterCue& cue : m_bosses.takeCues()) {
        showCritterCue(cue, m_bosses.archive(), true);
    }
    for (const CritterCue& cue : m_critters.takeCues()) {
        showCritterCue(cue, m_critters.archiveOf(cue.critter), false);
    }
    followCritterEffects();
    for (const CritterBlow& blow : m_critters.takeBlows()) {
        for (usize i = 0; i < m_actors.size(); ++i) {
            if (m_actors[i].player() == blow.player && !isDown(i)) {
                hurt(i, blow.damage, HurtKind::Blow, true);
            }
        }
    }
    awardCritterLosses();
    updateLevels();
    for (const EnemyBlow& blow : m_enemies.takeBlows()) {
        for (usize i = 0; i < m_actors.size(); ++i) {
            if (m_actors[i].player() != blow.player || isDown(i)) {
                continue;
            }
            const bool guarding = m_figures[i] != nullptr && m_figures[i]->animator.guarding();
            if (blow.knocksDown && !guarding && m_struck[i] == PlayerDeed::None) {
                m_struck[i] = PlayerDeed::Flinch;
            }
            hurt(i, blow.damage, HurtKind::Blow, true);
        }
    }
    for (const EnemyLoss& loss : m_enemies.takeLosses()) {
        awardExperience(loss.player, loss.experience, loss.killed);
    }
}

/** A hit on one of the swarm, from a player or the world. */
void PlayScene::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer) {
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    for (const PlayerActor& actor : m_actors) {
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    m_enemies.hurt(id, hit);
}

/** A hit on a generator: as it crumbles a state its kind's hit or death effect plays over
 * it to the realm's own sound (`S_GENDAMG`, `S_GENKILLG`), and, gone, its brood is freed of
 * it. */
void PlayScene::strikeGenerator(s32 id, f32 power, s32 byPlayer) {
    const auto event = m_generators.strike(id, power, byPlayer);
    if (!event.has_value()) {
        return;
    }
    if (ItemArchive* archive = m_enemies.archive(event->kind); archive != nullptr && m_device != nullptr) {
        const std::string_view tree = event->destroyed ? "GENDIE" : "GENHIT";
        if (archive->trees.find(tree).has_value()) {
            m_effects.start(*m_device, *archive, tree, event->position);
        }
    }
    playRealmSound(event->destroyed ? "S_GENKILL" : "S_GENDAM");
    if (event->destroyed) {
        m_enemies.generatorGone(id);
    }
}

/** Gas hangs for a while and hurts whoever stands in it, every half second. */
void PlayScene::updateClouds(f32 seconds) {
    for (f32& gap : m_cloudGaps) {
        gap = std::max(gap - seconds, 0.0f);
    }
    for (GasCloud& cloud : m_clouds) {
        cloud.secondsLeft -= seconds;
        for (usize i = 0; i < m_actors.size() && i < m_cloudGaps.size(); ++i) {
            if (isDown(i) || m_cloudGaps[i] > 0.0f) {
                continue;
            }
            const Vec3 offset = m_actors[i].followPoint() - cloud.position;
            if (std::hypot(offset.x, offset.z) <= kGasRadius + m_actors[i].radius() &&
                std::abs(offset.y) <= m_actors[i].height() * 0.5f + kGasRadius) {
                m_cloudGaps[i] = kGasGapSeconds;
                hurt(i, cloud.damage, HurtKind::Gas, true);
            }
        }
    }
    std::erase_if(m_clouds, [](const GasCloud& cloud) { return cloud.secondsLeft <= 0.0f; });
}

/** A gate's opening sounds from the realm's own bank, named after the level's letter. */
void PlayScene::playGateSound(s32 /*subtype*/) {
    const std::string& level = m_world->ref().name;
    const char letter = level.empty() ? 'G' : level.front();
    for (const std::string_view stem : {"S_GATEMET", "S_GATEWOOD", "S_GATE"}) {
        for (const std::string_view tail : {"", "1"}) {
            if (playNamed(std::format("{}{}{}", stem, letter, tail)) != kNoSound) {
                return;
            }
        }
    }
}

/** Takes health from a character the way the original does: harm over a point is scaled by
 * the level's damage, the cry depends on what did it, and with under a point of health left
 * the character dies, to the dying sound and its own last cry. Nobody is hurt in the tower,
 * nor once they have fallen. */
void PlayScene::hurt(usize index, f32 damage, HurtKind kind, bool directed) {
    if (index >= m_actors.size() || isDown(index) || m_world->isTower() || damage <= 0.0f) {
        return;
    }
    const f32 unguarded = damage;
    damage = guarded(index, damage, directed);
    if (m_figures[index] != nullptr && m_figures[index]->animator.defending()) {
        showBlock(index, unguarded - damage, damage);
    }
    if (damage <= 0.0f) {
        return;
    }
    const LevelInfo* level = m_world->level();
    if (damage > 1.0f && level != nullptr) {
        damage *= level->tuning.damage;
    }
    CharacterSave& save = m_actors[index].save();
    const s32 left = save.health() - static_cast<s32>(std::lround(damage));
    if (left < 1) {
        // Health of nought would read as a class never played: the fallen keep a point that
        // the status box does not show.
        save.progress().health = 1;
        m_down[index] = kDying;
        m_turbo[index].reset();
        playNamed(kDeathSound);
        cry(index, "DIE2");
        log::info("Player {} has fallen", m_actors[index].player() + 1);
        return;
    }
    const s32 before = save.health();
    save.progress().health = left;
    // Crossing into low health is remarked on by name rather than cried over.
    if (before > kHealthLowMark && left <= kHealthLowMark) {
        sayWithName(index, kBadlyLine);
        return;
    }
    if (before > kHealthLastMark && left <= kHealthLastMark) {
        sayWithName(index, (m_lowHealthTurn++ % 2 == 0) ? kLifeForceLine : kAboutToDieLine);
        return;
    }
    switch (kind) {
    case HurtKind::Burn:
        cryPain(index);
        m_painOwed[index] = 0.0f;
        break;
    case HurtKind::Pierce: cry(index, "DIE1"); break;
    case HurtKind::Gas: cry(index, "POISON"); break;
    case HurtKind::Blow:
        // A heavy blow gets a cry at once; lesser ones add up to one, and land with the
        // sound of the hit itself now and then.
        m_painOwed[index] += damage;
        if (before - left > kHeavyBlow) {
            m_painOwed[index] = 0.0f;
            cryPain(index);
        } else if (m_painOwed[index] >= kPainEvery) {
            m_painOwed[index] -= kPainEvery;
            cryPain(index);
        } else if (m_hitSoundGaps[index] <= 0) {
            playNamed(kHitSound);
            m_hitSoundGaps[index] = kHitSoundGapTicks;
        }
        break;
    }
}

/** One of the character's four cries of pain, whichever comes. */
void PlayScene::cryPain(usize index) {
    const s32 which = 1 + static_cast<s32>(m_painRandom() % kPainCries);
    cry(index, std::format("PAIN{}", which));
}

/** The narrator names the character ("Red Warrior", from the class's own bank) and says
 * `line` after: what the original's announcements by name do. */
void PlayScene::sayWithName(usize index, std::string_view line) {
    Figure* body = index < m_figures.size() ? m_figures[index].get() : nullptr;
    if (body == nullptr || m_context.sounds == nullptr) {
        return;
    }
    const CharacterSave& save = m_actors[index].save();
    const std::string name =
        std::format("S_{}{}2", colorCode(save.color), classCode(save.character % kStartingClassCount));
    SoundHandle spoken = kNoSound;
    if (const auto sound = body->voice.find(name); sound.has_value()) {
        spoken = m_context.sounds->play(body->voice.sequence(*sound), 1.0f, SoundCategory::Effects);
    }
    for (SoundSet* bank : {&m_narrator, &m_narratorSecond}) {
        if (const auto sound = bank->find(line); sound.has_value()) {
            m_context.sounds->playAfter(spoken, bank->sequence(*sound), 1.0f,
                                        SoundCategory::Effects);
            return;
        }
    }
}

/** The whole party has gone through a portal: where to? Its own level when that is unpacked;
 * from a realm's level whose next is not, back to the tower, so that no one is stranded;
 * from the tower, nowhere, with a word in the log. */
bool PlayScene::leaveBy(usize portal) {
    const ExitPortals::Portal& exit = m_portals.portal(portal);
    const bool reachable = exit.destination.has_value() &&
                           LevelCatalog::unpacked(m_context.unpackedRoot, *exit.destination);
    if (reachable) {
        m_destination = *exit.destination;
        log::info("Portal {}: on to {} ({})", exit.tag, m_destination.name, m_destination.title);
        return true;
    }
    if (!m_world->isTower()) {
        m_destination = LevelRef::tower();
        log::info("Portal {}: its level is not unpacked; back to the tower", exit.tag);
        return true;
    }
    if (m_refusedPortal != static_cast<s32>(portal)) {
        m_refusedPortal = static_cast<s32>(portal);
        log::warn("Portal {}: its level is not unpacked; unpack it with gdlunpack --only "
                  "<level> (and its realm's items)",
                  exit.tag);
    }
    return false;
}

std::vector<PartyMember> PlayScene::party() const {
    std::vector<PartyMember> members;
    members.reserve(m_actors.size());
    // The fallen go on as they came into the level, less what it gave them (but what they
    // were taught stays taught).
    for (usize i = 0; i < m_actors.size(); ++i) {
        const bool down = isDown(i) && i < m_entrySaves.size();
        PartyMember member{m_actors[i].player(), down ? m_entrySaves[i] : m_actors[i].save(),
                           i < m_slots.size() ? m_slots[i] : std::nullopt, down};
        member.save.helpSeen = m_actors[i].save().helpSeen;
        if (i < m_helpHeard.size()) {
            member.helpHeard = m_helpHeard[i];
        }
        members.push_back(std::move(member));
    }
    return members;
}

/** The bottles potions fly as, from the weapons archive the spawn effect came from. */
void PlayScene::loadPotionModels(RenderDevice& device) {
    if (!m_weapons.loaded()) {
        return;
    }
    for (usize kind = 0; kind < m_potionModels.size(); ++kind) {
        if (const auto tree = m_weapons.trees.find(kPotions[kind].bottle); tree.has_value()) {
            m_potionModels[kind].bind(m_weapons.trees.tree(*tree), m_weapons.models,
                                      m_weapons.textures, device);
        }
    }
}

/** A potion's magic going off at `position`: its burst, as large as the magic power behind
 * it makes it (full size at thirty-two), and its sound. */
void PlayScene::burstPotion(s32 kind, const Vec3& position, f32 power) {
    const PotionLook& look = potionLook(kind);
    if (m_device != nullptr && m_weapons.loaded()) {
        m_effects.start(*m_device, m_weapons, look.burst, position,
                        std::min(kBurstPerPower * power, 1.0f));
    }
    playNamed(look.sound);
}

f32 PlayScene::magicPowerOf(const PlayerActor& actor) const {
    const CharacterSave& save = actor.save();
    const ClassStats* stats = m_classes.stats(save.character);
    const s32 magic =
        stats != nullptr
            ? displayStats(*stats, experienceLevel(save.experience()), save.progress()).magic()
            : 0;
    return PowerupEffects::of(save.progress().inventory).magicPower(magic);
}

/** The next potion carried goes off about the character. */
void PlayScene::usePotion(PlayerActor& actor) {
    if (const s32 kind = actor.save().progress().inventory.takePotion(); kind != 0) {
        burstPotion(kind, actor.position(), magicPowerOf(actor));
    }
}

/** The next potion carried is tossed ahead, as much up as forwards, to burst where it
 * lands. */
void PlayScene::throwPotion(PlayerActor& actor) {
    const s32 kind = actor.save().progress().inventory.takePotion();
    if (kind == 0) {
        return;
    }
    const Vec3 facing = actor.facing();
    MissileLaunch launch;
    launch.owner = actor.player();
    launch.direction = facing;
    launch.position =
        actor.position() + facing * kPotionHandReach + Vec3{0.0f, kPotionHandHeight, 0.0f};
    launch.velocity = Vec3{facing.x * kPotionLoft, kPotionLoft, facing.z * kPotionLoft} *
                      kPotionToss;
    launch.potion = kind;
    launch.potency = kThrownShare * magicPowerOf(actor);
    launch.spec = &MissileSpec::potion();
    launch.model = &m_potionModels[static_cast<usize>(
        std::clamp(kind, 0, static_cast<s32>(m_potionModels.size()) - 1))];
    m_missiles.launch(launch);
}

f32 PlayScene::bodyScale(const CharacterSave& save, const PowerupEffects& effects) {
    if (save.character == kOgre) {
        return kOgreScale;
    }
    if (effects.grown()) {
        return PowerupEffects::kGrowthScale;
    }
    return experienceLevel(save.experience()) >= kMaxLevel ? kMasterScale : 1.0f;
}

/** Works a player's powerup selector with this frame's presses, sounding as the original's
 * does: the menu's up-and-down note for opening and closing, its sideways note for going
 * round, its select for taking one off or putting it on. */
void PlayScene::stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks) {
    const auto slot = static_cast<usize>(std::clamp(actor.player(), 0, kPlayerCount - 1));
    switch (m_selectors[slot].step(input, actor.save().progress().inventory, ticks)) {
    case SelectorCue::Opened:
    case SelectorCue::Closed: playNamed(kMenuMoveSound); break;
    case SelectorCue::Moved: playNamed(kSelectorMoveSound); break;
    case SelectorCue::Switched: playNamed(kMenuSelectSound); break;
    case SelectorCue::None: break;
    }
}

/** The name of the powerup each open selector has come to, over its player's box: glowing
 * while the powerup is worn, plain once taken off. */
void PlayScene::drawSelectors() {
    if (!m_text.ready() || m_context.strings == nullptr) {
        return;
    }
    for (const PlayerActor& actor : m_actors) {
        const PowerupSelector& selector = this->selector(actor.player());
        const s32 chosen = selector.selection();
        if (!selector.showing() || chosen < 0) {
            continue;
        }
        const PowerupSlot& slot =
            actor.save().progress().inventory.powerups[static_cast<usize>(chosen)];
        const std::string_view label =
            m_context.strings->get(powerupTextId(slot.kind, slot.flags));
        const s32 x = actor.player() * StatusBoxPainter::kWidth + PowerupSelector::kLabelX;
        const s32 y = selector.labelY(StatusBoxPainter::kY);
        TextStyle style;
        style.scale = PowerupSelector::kLabelScale;
        if (slot.on && m_glowSheet != nullptr) {
            TextStyle glow = style;
            glow.texture = m_glowSheet;
            glow.color = ScrollBox::kGlowColor;
            glow.expand = OptionMenu::kGlowExpand;
            m_text.draw(m_canvas, x, y, label, glow);
        }
        m_text.draw(m_canvas, x, y, label, style);
    }
}

/** Hands a touched item to whoever touched it, by the original's rules: their card slides
 * up, the item's sound (or their own eating) plays, and what they cannot carry stays lying
 * where it is. Crystals are the party's and are dealt with once taken. */
std::optional<s32> PlayScene::takePickup(const Pickup& pickup) {
    if (pickup.realm > 0) {
        return 0;
    }
    if (pickup.collector >= m_actors.size()) {
        return std::nullopt;
    }
    PlayerActor& actor = m_actors[pickup.collector];
    const ClassStats* stats = m_classes.stats(actor.save().character);
    const ItemTaking taking =
        takeItem(actor.save(), ItemOffer{pickup.subtype, pickup.amount, pickup.flags,
                                         pickup.strength},
                 stats != nullptr ? stats->powerupTime : 1.0f);
    if (!taking.took()) {
        if (taking.outcome == ItemTaking::Outcome::KeysFull) {
            postHelp(HelpMessages::kKeysFull, pickup.collector);
        } else if (taking.outcome == ItemTaking::Outcome::HealthFull) {
            postHelp(HelpMessages::kHealthFull, pickup.collector);
        } else if (taking.outcome == ItemTaking::Outcome::AlreadyHeld) {
            postHelp(HelpMessages::kAlreadyHaveRune, pickup.collector);
        }
        return std::nullopt;
    }
    if (pickup.subtype == kSpecialPowerup && (static_cast<u32>(pickup.flags) & kTurboFlag) != 0) {
        m_turbo[pickup.collector].add(TurboMeter::kFull);
    }
    switch (static_cast<ItemKind>(pickup.subtype)) {
    case ItemKind::Runestone: shareRune(pickup.amount); break;
    case ItemKind::Legend:
        postHelp(HelpMessages::kFirstLegendName + taking.count, pickup.collector);
        break;
    case ItemKind::Scroll:
        if (const LevelInfo* level = m_world->level(); level != nullptr && taking.count >= 0) {
            openMessage(std::format("{}{}", kLevelScrollPrefix, level->name),
                        static_cast<usize>(taking.count));
        }
        break;
    default: break;
    }
    if (!taking.card.empty()) {
        m_pickups.addCard(actor.player(), taking.card);
    }
    if (!taking.sound.empty()) {
        playNamed(taking.sound);
    } else if (Figure* figure = m_figures[pickup.collector].get();
               figure != nullptr && m_context.sounds != nullptr) {
        const std::string_view voice = classCode(actor.save().character % kStartingClassCount);
        const auto sound = figure->voice.find(
            std::format("S_{}{}", voice, taking.hurt ? "PAIN1" : "EATSFX"));
        if (sound.has_value()) {
            m_context.sounds->play(figure->voice.sequence(*sound), 1.0f, SoundCategory::Effects);
        }
    }
    return taking.left;
}

/** A runestone found is everyone's: each character in play gets it, and the narrator counts
 * what the party holds. */
void PlayScene::shareRune(s32 rune) {
    u16 held = 0;
    for (PlayerActor& actor : m_actors) {
        Relics& relics = actor.save().progress().relics;
        relics.addRune(rune);
        held |= relics.runes;
    }
    const s32 count = std::popcount(held);
    if (count <= 0) {
        return;
    }
    const std::string voice =
        count == 1 ? std::string(kFirstRuneVoice) : std::format("{}{}", kRuneVoicePrefix, count);
    if (m_context.sounds != nullptr) {
        if (const auto line = m_narrator.find(voice); line.has_value()) {
            m_context.sounds->play(m_narrator.sequence(*line), 1.0f, SoundCategory::Effects);
        }
    }
}

/** Loops the level's music stream from the game's files at the level's volume. */
void PlayScene::startMusic() {
    const LevelAudioInfo* audio = m_world->audio();
    if (m_context.sounds == nullptr || m_context.assets == nullptr || audio == nullptr ||
        audio->stream.empty()) {
        return;
    }
    const auto file =
        m_context.assets->find(std::format("{}/{}.ads", kStreamsDirectory, audio->stream));
    if (!file.has_value()) {
        log::warn("Tower: music stream {} is not among the game's files", audio->stream);
        return;
    }
    auto stream = std::make_shared<AdsStream>();
    if (!stream->open(*file)) {
        return;
    }
    const LevelInfo* level = m_world->level();
    m_music = m_context.sounds->playStream(std::move(stream), true,
                                          level != nullptr ? level->musicVolume : 1.0f,
                                          SoundCategory::Music);
}

void PlayScene::playStep(PlayerAnimator::Foot foot) {
    playCommon(m_stepSounds[foot == PlayerAnimator::Foot::Second ? 1 : 0]);
}

/** Gathers the scroll's art: the sheet, the prompt's font and glow, the button icon and the
 * burn frames, and the scroll texts. Missing pieces only lose the welcome's scroll. */
void PlayScene::loadIntroArt(RenderDevice& device) {
    ScrollBoxArt art;
    m_text.setFont(nullptr, nullptr);
    if (!m_staticTextures.load(m_context.unpackedRoot / kStaticDirectory) ||
        !m_font32.load(m_context.unpackedRoot / kFontFile, kFont32SpaceWidth) ||
        !m_scrollText.load(m_context.unpackedRoot / kScrollTextFile)) {
        log::warn("Tower: the scroll's art or texts are not unpacked; the welcome is skipped");
        m_scroll.setArt(art);
        return;
    }
    if (m_context.strings != nullptr) {
        m_scrollText.translate(*m_context.strings, kScrollTextPrefix);
    }
    const auto texture = [&](std::string_view name, u32 frame = 0) -> const Texture* {
        const auto index = m_staticTextures.find(name);
        if (!index.has_value() || *index + frame >= m_staticTextures.size()) {
            return nullptr;
        }
        try {
            return &m_staticTextures.texture(device, *index + frame);
        } catch (const std::exception& e) {
            log::warn("Tower: texture {}: {}", name, e.what());
            return nullptr;
        }
    };
    const Texture* font = texture(kFontTexture);
    if (font != nullptr) {
        m_text.setFont(&m_font32, font);
    }
    art.backdrop = texture(kScrollTexture);
    art.glow = texture(kGlowTexture);
    art.button = texture(kButtonTexture);
    const auto scroll = m_staticTextures.find(kScrollTexture);
    const auto ring = m_staticTextures.find(kFireRingTexture);
    const auto mask = m_staticTextures.find(kFireMaskTexture);
    if (scroll.has_value() && ring.has_value() && mask.has_value()) {
        try {
            art.backdropImage = &m_staticTextures.image(*scroll);
            const auto frames = static_cast<u32>(BurnDialogueScroll::kFrameCount);
            for (u32 i = 1; i <= frames && *ring + i < m_staticTextures.size(); ++i) {
                art.burnRing.push_back(&m_staticTextures.texture(device, *ring + i));
            }
            for (u32 i = 1; i <= frames && *mask + i < m_staticTextures.size(); ++i) {
                art.burnMasks.push_back(&m_staticTextures.image(*mask + i));
            }
        } catch (const std::exception& e) {
            log::warn("Tower: burn frames: {}", e.what());
            art.backdropImage = nullptr;
            art.burnRing.clear();
            art.burnMasks.clear();
        }
    }
    m_scroll.setText(&m_text);
    m_scroll.setArt(std::move(art));
}

/** What Sumner's scroll of hints draws with: the menu sheets and the scroll from the static
 * archive, the selection arrow from the powerups, and his hints' texts. */
void PlayScene::loadHintArt(RenderDevice& device) {
    HintMenuArt art;
    if (!m_staticTextures.loaded() ||
        !m_hints.load(m_context.unpackedRoot / kHintTextFile)) {
        log::warn("Tower: Sumner's hints are not unpacked; he has nothing to say");
        m_hintMenu.setArt(std::move(art));
        return;
    }
    if (m_context.strings != nullptr) {
        m_hints.translate(*m_context.strings);
    }
    const auto texture = [&](std::string_view name) -> const Texture* {
        const auto index = m_staticTextures.find(name);
        try {
            return index.has_value() ? &m_staticTextures.texture(device, *index) : nullptr;
        } catch (const std::exception& e) {
            log::warn("Tower: texture {}: {}", name, e.what());
            return nullptr;
        }
    };
    art.textures.font = texture(kFontTexture);
    art.textures.glow = texture("FONT32_GLOW");
    m_glowSheet = art.textures.glow;
    art.textures.parchment = texture("FONT32_PARCH");
    art.textures.arrows = texture("ARROWS");
    for (usize i = 0; i < art.textures.garamond.size(); ++i) {
        art.textures.garamond[i] = texture(std::format("FONT32GAR{}", i));
    }
    art.textures.backdrop = texture(kScrollTexture);
    ItemArchive& powerups = m_world->powerups();
    if (const auto tree = powerups.trees.find(kArrowTree);
        powerups.loaded() && tree.has_value() &&
        m_hintArrow.bind(powerups.trees.tree(*tree), powerups.models, powerups.textures,
                         device)) {
        art.textures.icon = &m_hintArrow;
    }
    const auto scroll = m_staticTextures.find(kScrollTexture);
    const auto ring = m_staticTextures.find(kFireRingTexture);
    const auto mask = m_staticTextures.find(kFireMaskTexture);
    if (scroll.has_value() && ring.has_value() && mask.has_value()) {
        try {
            art.scroll = &m_staticTextures.image(*scroll);
            const auto frames = static_cast<u32>(BurnDialogueScroll::kFrameCount);
            for (u32 i = 1; i <= frames && *ring + i < m_staticTextures.size(); ++i) {
                art.burnRing.push_back(&m_staticTextures.texture(device, *ring + i));
            }
            for (u32 i = 1; i <= frames && *mask + i < m_staticTextures.size(); ++i) {
                art.burnMasks.push_back(&m_staticTextures.image(*mask + i));
            }
        } catch (const std::exception& e) {
            log::warn("Tower: burn frames: {}", e.what());
            art.scroll = nullptr;
        }
    }
    m_hintMenu.setArt(std::move(art));
}

/** The first of the party standing in the spot before Sumner, or null. */
const PlayerActor* PlayScene::visitorOfSumner() const {
    const LevelTriggers& triggers = m_world->triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& spot = triggers.trigger(i);
        if (spot.id != kSumnerSpot) {
            continue;
        }
        for (const PlayerActor& actor : m_actors) {
            const Vec3 away = actor.position() - spot.spot;
            const f32 reach = spot.radius + actor.radius();
            if (away.x * away.x + away.z * away.z <= reach * reach &&
                std::abs(away.y) <= LevelTriggers::kReach) {
                return &actor;
            }
        }
    }
    return nullptr;
}

/** A player stepping up to Sumner is greeted at once and handed his scroll of hints two
 * seconds on, once a visit; stepping away and back is a new visit. */
void PlayScene::updateSumnerVisit(f32 seconds) {
    if (m_greetingLeft > 0.0f) {
        m_greetingLeft = std::max(m_greetingLeft - seconds, 0.0f);
    }
    const PlayerActor* visitor = visitorOfSumner();
    if (visitor == nullptr) {
        m_hintsGiven = false;
        return;
    }
    if (m_hintsGiven || !m_hints.loaded() || !m_sumner.loaded()) {
        return;
    }
    if (m_greetingLeft < 0.0f) {
        m_sumner.play(SumnerFigure::kWelcomeIndex);
        m_greetingLeft = kGreetingSeconds;
    } else if (m_greetingLeft == 0.0f) {
        m_greetingLeft = -1.0f;
        m_hintsGiven = true;
        openHints(visitor->player());
    }
}

void PlayScene::openHints(s32 player) {
    if (m_context.strings == nullptr || m_context.config == nullptr) {
        return;
    }
    const StringTable& strings = *m_context.strings;
    HintMenuLabels labels;
    labels.title = std::string(strings.get("hints.title"));
    for (const HintTopicEntry& topic : kHintTopics) {
        labels.topics.push_back(MenuItem{std::string(strings.get(topic.text)), topic.code});
    }
    labels.back = std::string(strings.get("menu.back"));
    labels.select = std::string(strings.get("menu.select"));
    labels.player = std::string(strings.get("menu.player"));
    if (const auto slot = labels.player.find("{}"); slot != std::string::npos) {
        labels.player.replace(slot, 2, std::to_string(player + 1));
    }
    MenuScreen screen;
    screen.width = static_cast<s32>(m_context.config->display.virtualWidth);
    screen.height = static_cast<s32>(m_context.config->display.virtualHeight);
    screen.horizontalFov = m_context.config->horizontalFovRadians();
    m_hints.beginVisit();
    if (m_hintMenu.open(m_text, screen, std::move(labels))) {
        m_hintPlayer = player;
    }
}

/** Steps the scroll of hints with its player's input; backing out of it burns it and has
 * Sumner wave the player off. */
void PlayScene::updateHints(const Inputs& inputs, s32 ticks) {
    const auto player = static_cast<usize>(std::max(m_hintPlayer, 0));
    const MenuInput input = player < inputs.size() ? inputs[player].menu : MenuInput{};
    const HintMenuEvent event = m_hintMenu.update(*m_device, input, ticks);
    switch (event.kind) {
    case HintMenuEvent::Kind::Moved:
        playNamed(kMenuMoveSound);
        break;
    case HintMenuEvent::Kind::Asked:
        playNamed(kMenuSelectSound);
        answerHint(event.topic);
        break;
    case HintMenuEvent::Kind::Returned:
        playNamed(kMenuExitSound);
        break;
    case HintMenuEvent::Kind::Left:
        playNamed(m_hintMenu.burning() ? kScrollBurnSound : kMenuExitSound);
        m_sumner.play(SumnerFigure::kGoAwayIndex);
        break;
    case HintMenuEvent::Kind::None:
        break;
    }
    if (!m_hintMenu.active()) {
        m_hintPlayer = -1;
    }
}

void PlayScene::answerHint(s32 topic) {
    const auto entry = std::ranges::find(kHintTopics, topic, &HintTopicEntry::code);
    if (entry == kHintTopics.end() || m_context.strings == nullptr) {
        return;
    }
    std::vector<ClassProgress> party;
    party.reserve(m_actors.size());
    for (const PlayerActor& actor : m_actors) {
        party.push_back(actor.save().progress());
    }
    HintPage page = m_hints.next(entry->topic, HintKnowledge::ofParty(party),
                                 m_context.strings->get("hints.generalTitle"));
    m_hintMenu.read(m_text, std::move(page.title), std::move(page.passages), page.scale,
                    page.centred, page.gap);
}

/** A party is new to the tower while no class of any of its characters has experience. */
bool PlayScene::freshParty(std::span<const PartyMember> party) {
    return !party.empty() && std::ranges::all_of(party, [](const PartyMember& member) {
        return std::ranges::none_of(member.save.classes, [](const ClassProgress& progress) {
            return progress.experience > 0;
        });
    });
}

/** Opens Sumner's welcome scroll; without it the welcome goes straight to the crystals. */
void PlayScene::beginIntro(RenderDevice& device) {
    const auto message = m_scrollText.loaded() ? m_scrollText.find(kWelcomeMessage) : std::nullopt;
    if (message.has_value()) {
        const MessageInfo& welcome = m_scrollText.message(*message);
        const std::string prompt =
            m_context.strings != nullptr ? std::string(m_context.strings->get(kPromptText)) : "";
        if (m_scroll.open(device, welcome.pages, welcome.scale, prompt)) {
            m_intro = Intro::Scroll;
            return;
        }
    }
    log::warn("Tower: no welcome scroll to show; on to the crystals");
    startCrystalCut();
}

/** Sumner gestures at the crystals while the camera cuts to them from the level's marker. */
void PlayScene::startCrystalCut() {
    m_sumner.gesture();
    const WorldLocator* marker =
        m_world->layout().findLocator(LocatorKind::TriggerCamera, kCrystalCamera);
    if (marker == nullptr) {
        log::warn("Tower: no crystal camera marker {}", kCrystalCamera);
        m_intro = Intro::Done;
        return;
    }
    m_cutCamera = WorldCamera{};
    m_cutCamera.position = marker->position;
    m_cutCamera.pitch = marker->rotation.x;
    m_cutCamera.yaw = marker->rotation.y;
    m_cutTicks = kCrystalTicks;
    m_intro = Intro::Crystal;
}

/** A bit per party member whose player pressed their button this frame. */
u32 PlayScene::acceptedPlayers(const Inputs& inputs) const {
    u32 accepted = 0;
    for (const PlayerActor& actor : m_actors) {
        const auto player = static_cast<usize>(actor.player());
        if (player < inputs.size() && inputs[player].menu.select) {
            accepted |= 1U << player;
        }
    }
    return accepted;
}

const WorldCamera& PlayScene::viewCamera() const {
    if (m_startCamera.active()) {
        return m_startCamera.camera();
    }
    if (m_intro == Intro::Crystal) {
        return m_cutCamera;
    }
    return bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
}

/** A boss level with a boss camera record frames the fight with it while the boss stands. */
bool PlayScene::bossCameraOn() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    return level != nullptr && level->bossCamera.has_value() &&
           (m_bosses.present() || m_victory.running());
}

/** The boss as the camera sees it; once it has fallen, the wizard in its place. */
BossCameraSubject PlayScene::bossSubject() const {
    BossCameraSubject subject;
    if (m_victory.running() && !m_bosses.present()) {
        subject.position = m_wizardPosition;
        subject.facing = m_wizardYaw;
        subject.height = kWizardLift;
        subject.awake = true;
        return subject;
    }
    if (const Vec3* at = m_bosses.position(); at != nullptr) {
        subject.position = *at;
    }
    subject.facing = m_bosses.facing();
    subject.radius = m_bosses.radius();
    subject.height = m_bosses.height();
    subject.awake = m_bosses.view().awake;
    return subject;
}

/** Whether any party member's player pressed a button this frame. */
bool PlayScene::anyButton(const Inputs& inputs) const {
    return std::ranges::any_of(m_actors, [&inputs](const PlayerActor& actor) {
        const auto player = static_cast<usize>(actor.player());
        return player < inputs.size() && (inputs[player].menu.select ||
                                          inputs[player].menu.back || inputs[player].menu.start);
    });
}

void PlayScene::Figure::animate(f32 stickMagnitude, s32 ticks, f32 seconds, PlayerDeed deed) {
    if (!animator.bound()) {
        return;
    }
    animator.update(PlayerAnimator::motionFor(stickMagnitude), ticks, seconds, deed);
    const std::span<const Mat4> matrices = animator.pose().matrices();
    transforms.resize(costume->nodes.size());
    for (usize n = 0; n < transforms.size(); ++n) {
        const s32 source = classNodeOfNode[n];
        transforms[n] = source >= 0 && static_cast<usize>(source) < matrices.size()
                            ? matrices[static_cast<usize>(source)]
                            : glm::translate(Mat4{1.0f}, costume->worldPosition(n));
    }
}

PlayOutcome PlayScene::update(f64 deltaSeconds, const Inputs& inputs) {
    if (!m_open) {
        return PlayOutcome::Running;
    }
    // The clock advances in whole ticks, two per frame at the 30 frames per second the game
    // runs at, so a late frame moves everything further rather than smoother.
    const f32 tickRate =
        m_context.config != nullptr ? static_cast<f32>(m_context.config->timing.tickRate) : 60.0f;
    const auto ticks = std::clamp(static_cast<s32>(std::lround(deltaSeconds * tickRate)),
                                  kMinTicks, kMaxTicks);
    const f32 seconds = static_cast<f32>(ticks) / tickRate;
    // A scroll holds everything else still until it has burnt away; the welcome's leads on
    // to the crystals. Leaving one burns it to the options menu's note and cuts off whatever
    // Sumner was saying over it.
    if (m_scroll.active()) {
        const bool wasBurning = m_scroll.burning();
        m_scroll.step(ticks, acceptedPlayers(inputs));
        if ((m_scroll.burning() && !wasBurning) || !m_scroll.active()) {
            stopVoice();
            if (!wasBurning) {
                playNamed(kScrollBurnSound);
            }
        }
        if (!m_scroll.active() && m_intro == Intro::Scroll) {
            startCrystalCut();
        }
        return PlayOutcome::Running;
    }
    // Gone through a portal, the party is out of play while the transition picture comes
    // up over the level, which goes on around it; once it covers the view they travel.
    m_transition.update(seconds);
    if (m_leaving) {
        m_world->update(seconds);
        m_effects.update(seconds);
        updateAmbience();
        return m_transition.covering() ? PlayOutcome::Travel : PlayOutcome::Running;
    }
    // Sumner's scroll of hints holds play the same way, while he goes on moving behind it.
    if (m_hintMenu.active()) {
        updateHints(inputs, ticks);
        m_sumner.update(seconds);
        return PlayOutcome::Running;
    }
    // Materialising, the party stands still, playing its entrance, while the level runs on
    // around it and the start camera holds, then rides in; the title slides up until the
    // ride, when it sits.
    if (spawning()) {
        updateSpawn(ticks, seconds);
        for (const std::unique_ptr<Figure>& figure : m_figures) {
            if (figure != nullptr) {
                figure->animate(0.0f, ticks, seconds);
            }
        }
        m_world->update(seconds);
        updateAmbience();
        updateBeam(ticks);
        m_spawnTicks = std::max(m_spawnTicks - ticks, 0);
        m_startCamera.update(ticks, anyButton(inputs), m_camera.camera().position,
                             m_camera.attention());
        m_titleSlide += kTitleSlideRate * static_cast<f32>(ticks);
        if (m_startCamera.phase() != StartCamera::Phase::Hold || m_titleSlide > kTitleSlideEnd) {
            m_titleSlide = kTitleSlideEnd;
        }
        if (!spawning() && m_welcomePending) {
            m_welcomePending = false;
            beginIntro(*m_device);
        }
        return PlayOutcome::Running;
    }
    const bool held = m_intro == Intro::Crystal;
    if (held) {
        m_cutTicks -= ticks;
        if (m_cutTicks <= 0) {
            m_intro = Intro::Done;
        }
    }
    m_world->update(seconds);
    m_world->revealCrystals(seconds);
    m_pickups.step(ticks, seconds);
    m_sumner.update(seconds);
    const f32 cameraYaw = m_camera.yaw();
    for (usize i = 0; i < m_actors.size(); ++i) {
        PlayerActor& actor = m_actors[i];
        const auto player = static_cast<usize>(actor.player());
        const bool down = isDown(i);
        // Reeling from a hit, a character neither moves nor does anything.
        const bool reeling = m_struck[i] != PlayerDeed::None ||
                             (m_figures[i] != nullptr && m_figures[i]->animator.reacting());
        const MoveInput& move = !held && !down && !reeling && player < inputs.size()
                                    ? inputs[player].move
                                    : MoveInput{};
        // What the buttons ask: a potion first, when one is carried, then the attack.
        PlayerDeed deed = down ? PlayerDeed::Die : PlayerDeed::None;
        if (!down && m_struck[i] != PlayerDeed::None) {
            deed = m_struck[i];
        }
        m_struck[i] = PlayerDeed::None;
        if (!held && !down && !reeling && player < inputs.size()) {
            const PlayInput& in = inputs[player];
            const bool carrying = !actor.save().progress().inventory.potions.empty();
            if ((in.usePotion || in.throwPotion) && !carrying) {
                postHelp(HelpMessages::kNoPotion, i);
            }
            if ((in.shieldPotion) && !carrying) {
                postHelp(HelpMessages::kNoPotion, i);
            }
            if (const PlayerDeed turbo = turboDeed(i, in); turbo != PlayerDeed::None) {
                deed = turbo;
            } else if (in.shieldPotion && carrying) {
                deed = PlayerDeed::ShieldPotion;
            } else if (in.usePotion && carrying) {
                deed = PlayerDeed::UsePotion;
            } else if (in.throwPotion && carrying) {
                deed = PlayerDeed::ThrowPotion;
            } else if (in.strongAttack &&
                       m_figures[i]->animator.canBegin(PlayerDeed::StrongAttack)) {
                deed = PlayerDeed::StrongAttack; // nothing is ever in reach yet: the strong throw
            } else if (in.turbo) {
                deed = PlayerDeed::Defend; // held by itself, the turbo button is the guard
            } else if (in.attack) {
                deed = PlayerDeed::Attack;
            }
            stepSelector(actor, in.selector, ticks);
        }
        actor.setPaceBonus(PowerupEffects::of(actor.save().progress().inventory).paceAdd);
        // A body in a throw keeps its feet where they are, turning to the stick.
        const f32 pace = m_figures[i] != nullptr ? m_figures[i]->animator.moveScale() : 1.0f;
        const bool charging = m_figures[i] != nullptr && m_figures[i]->animator.shoving();
        // Strafing, the character steps the way the stick is pushed without turning to it.
        const bool strafes = !held && !down && !charging && player < inputs.size() &&
                             inputs[player].strafe && move.any();
        if (m_figures[i] != nullptr) {
            m_figures[i]->animator.setStrafe(
                strafes ? strafeWayOf(PlayerActor::headingOf(move, cameraYaw), actor.yaw())
                        : StrafeWay::None);
        }
        actor.update(charging ? chargeInput(i, move, cameraYaw) : move, cameraYaw, seconds,
                     &m_world->collision(), pace, strafes);
        if (charging) {
            ramBarrels(i);
        } else {
            m_rammed[i].clear();
        }
        if (m_figures[i] != nullptr) {
            m_figures[i]->animate(move.magnitude, ticks, seconds, deed);
            updateTurbo(i, ticks, seconds);
            if (m_down[i] == kDying && m_figures[i]->animator.dead()) {
                m_down[i] = kInTower; // the body goes; its box says where
            }
            if (m_figures[i]->animator.released()) {
                throwWeapon(actor, *m_figures[i]);
            }
            if (m_figures[i]->animator.strongReleased()) {
                launchWeapon(i, actor.facing(), kStrongThrowScale, true);
            }
            m_blockLeft[i] = std::max(m_blockLeft[i] - seconds, 0.0f);
            if (m_figures[i]->animator.potionShielded()) {
                shieldPotion(i);
            }
            if (m_figures[i]->animator.potionUsed()) {
                usePotion(actor);
            } else if (m_figures[i]->animator.potionThrown()) {
                throwPotion(actor);
            }
            if (const PlayerAnimator::Foot foot = m_figures[i]->animator.footfall();
                foot != PlayerAnimator::Foot::None) {
                playStep(foot);
            }
        }
        if (m_figures[i] == nullptr && m_down[i] == kDying) {
            m_down[i] = kInTower; // nothing to play: gone at once
        }
        m_subjects[i] = CameraSubject{actor.position(), actor.followPoint()};
    }
    m_playSeconds += seconds;
    m_help.update(ticks);
    updateFixtures(ticks, seconds);
    updateEnemies(ticks, seconds);
    std::vector<MissileTarget> targets;
    for (usize barrel = 0; barrel < m_barrels.size(); ++barrel) {
        if (m_barrels.standing(barrel)) {
            const Breakables::Barrel& cask = m_barrels.barrel(barrel);
            targets.push_back(MissileTarget{static_cast<s32>(barrel), cask.figure.position(),
                                            cask.radius, cask.height});
        }
    }
    for (MissileTarget target : m_enemies.targets()) {
        target.id += kEnemyTargetBase;
        targets.push_back(target);
    }
    for (MissileTarget target : m_critters.targets()) {
        target.id += kCritterTargetBase;
        targets.push_back(target);
    }
    for (MissileTarget target : m_bosses.targets()) {
        target.id += kBossTargetBase;
        targets.push_back(target);
    }
    for (usize g = 0; g < m_generators.count(); ++g) {
        if (m_generators.standing(static_cast<s32>(g))) {
            const Obstacle& box = m_generators.boxOf(static_cast<s32>(g));
            targets.push_back(MissileTarget{static_cast<s32>(g) + kGeneratorTargetBase,
                                            m_generators.positionOf(static_cast<s32>(g)),
                                            std::max(box.halfAcross, box.halfAlong), box.height});
        }
    }
    m_missiles.update(seconds, &m_world->collision(), targets);
    for (const MissileImpact& impact : m_missiles.takeImpacts()) {
        if (impact.potion != 0) {
            burstPotion(impact.potion, impact.position, impact.potency); // weapons leave no mark yet
        }
        if (impact.target >= kBossTargetBase) {
            EnemyHit hit;
            hit.damage = impact.damage;
            hit.player = impact.owner;
            hit.where = impact.position;
            for (const PlayerActor& actor : m_actors) {
                if (actor.player() == impact.owner) {
                    hit.direction = impact.position - actor.position();
                    hit.direction.y = 0.0f;
                    hit.level = experienceLevel(actor.save().experience());
                }
            }
            m_bosses.hurt(hit);
        } else if (impact.target >= kCritterTargetBase) {
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerActor& actor : m_actors) {
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            strikeCritter(impact.target - kCritterTargetBase, impact.damage, 0, direction, impact.owner,
                          impact.position);
        } else if (impact.target >= kGeneratorTargetBase) {
            strikeGenerator(impact.target - kGeneratorTargetBase, impact.damage, impact.owner);
        } else if (impact.target >= kEnemyTargetBase) {
            // The hit travels the way the weapon flew: out from whoever threw it.
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerActor& actor : m_actors) {
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            strikeEnemy(impact.target - kEnemyTargetBase, impact.damage, 0, direction, impact.owner);
        } else if (impact.target >= 0) {
            strikeBarrel(static_cast<usize>(impact.target), impact.damage, impact.owner);
            settleBlasts();
        }
    }
    updateStrikes(seconds);
    updateShields(seconds);
    m_dimmer.update(seconds);
    m_world->setAmbientOffset(m_dimmer.offset());
    m_effects.update(seconds);
    collectItems();
    updateBeam(ticks);
    m_world->updateTriggers(seconds, visitors());
    handleTriggerEvents();
    if (!held && !m_scroll.active() && m_world->isTower()) {
        updateSumnerVisit(seconds);
    }
    if (!held) {
        // A portal waits for everyone still on their feet.
        std::vector<PortalVisitor> standing;
        standing.reserve(m_actors.size());
        for (usize i = 0; i < m_actors.size(); ++i) {
            if (!isDown(i)) {
                standing.push_back(PortalVisitor{m_actors[i].position(), m_actors[i].radius()});
            }
        }
        // With nobody left, and the last of them done falling, the party is taken back.
        const bool falling = std::ranges::find(m_down, kDying) != m_down.end();
        if (standing.empty() && !m_actors.empty() && !falling) {
            m_fallenSeconds += seconds;
            if (m_fallenSeconds >= kFallenSeconds) {
                return PlayOutcome::Fallen;
            }
        }
        if (const auto portal = m_portals.update(ticks, seconds, standing);
            portal.has_value() && leaveBy(*portal)) {
            m_leaving = true;
            m_transition.comeUp();
        }
    }
    // The camera keeps to those still standing, while anyone is.
    std::vector<CameraSubject> followed;
    for (usize i = 0; i < m_subjects.size(); ++i) {
        if (!isDown(i)) {
            followed.push_back(m_subjects[i]);
        }
    }
    if (bossCameraOn()) {
        m_bossCamera.update(bossSubject(), followed.empty() ? m_subjects : followed,
                            *m_world->level()->bossCamera, cameraView(), seconds);
    } else {
        m_camera.update(followed.empty() ? m_subjects : followed, m_world->cameraMarkers(),
                        m_world->cameraRange(), cameraView(), seconds);
    }
    updateAmbience();
    return PlayOutcome::Running;
}

/** Places the level's loops for the party, heard from the camera. */
void PlayScene::updateAmbience() {
    if (m_context.sounds == nullptr) {
        return;
    }
    std::vector<Vec3> listeners;
    listeners.reserve(m_actors.size());
    for (const PlayerActor& actor : m_actors) {
        listeners.push_back(actor.position());
    }
    const CameraFrame frame = CameraFrame::of(viewCamera());
    const LevelInfo* level = m_world->level();
    m_ambience.update(*m_context.sounds, listeners, AmbientEar{frame.position, frame.right},
                      level != nullptr ? level->soundVolume : 1.0f);
}

/** The party as the level's triggers see it. */
std::vector<TriggerVisitor> PlayScene::visitors() const {
    std::vector<TriggerVisitor> out;
    for (usize i = 0; i < m_actors.size(); ++i) {
        const PlayerActor& actor = m_actors[i];
        TriggerVisitor visitor;
        visitor.position = presenceOf(i);
        visitor.radius = actor.radius();
        visitor.crystals = actor.save().progress().crystals;
        out.push_back(visitor);
    }
    return out;
}

void PlayScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                        f32 frameHeight) {
    if (!m_open || m_context.config == nullptr) {
        return;
    }
    const GameConfig& config = *m_context.config;
    m_scroll.prepare(device);
    m_hintMenu.prepare(device);
    const Mat4 clip = viewCamera().clipTransform(config.horizontalFovRadians(), frameWidth,
                                                 frameHeight, frameProjection);
    m_world->draw(device, clip, viewCamera());
    m_sumner.draw(device, clip, m_world->lighting());
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_figures[i] != nullptr && (i >= m_down.size() || m_down[i] != kInTower)) {
            const Figure& figure = *m_figures[i];
            const PowerupEffects worn =
                PowerupEffects::of(m_actors[i].save().progress().inventory);
            const f32 size = bodyScale(m_actors[i].save(), worn);
            const Mat4 body = glm::scale(m_actors[i].transform(), Vec3{size, size, size});
            figure.model.draw(device, clip, body, m_world->lighting(), figure.transforms, nullptr,
                              worn.bodyAlpha(m_playSeconds));
            // The hand is empty while a throw is recovered from, unless what flies is not
            // what it holds.
            const bool thrown = figure.animator.recovering() ||
                                figure.animator.action() ==
                                    PlayerAnimator::Action::StrongThrowRecover;
            const bool hidden = i < m_moves.size() && m_moves[i].weaponHidden;
            const bool inHand =
                !hidden && (!thrown || MissileSpec::of(m_actors[i].save().character).staysInHand);
            if (figure.handNode >= 0 && figure.weapon.bound() && inHand) {
                const auto hand = static_cast<usize>(figure.handNode);
                const Mat4 wrist = hand < figure.transforms.size()
                                       ? figure.transforms[hand]
                                       : glm::translate(Mat4{1.0f}, figure.costume->worldPosition(hand));
                figure.weapon.draw(device, clip, body * wrist, m_world->lighting(), {}, nullptr,
                                   worn.bodyAlpha(m_playSeconds));
            }
        }
    }
    m_portals.draw(device, clip, m_world->lighting());
    m_chests.draw(device, clip, m_world->lighting());
    m_gates.draw(device, clip, m_world->lighting());
    m_traps.draw(device, clip, m_world->lighting());
    m_barrels.draw(device, clip, m_world->lighting());
    m_generators.draw(device, clip, m_world->lighting());
    m_enemies.draw(device, clip, m_world->lighting());
    m_critters.draw(device, clip, m_world->lighting());
    m_bosses.draw(device, clip, m_world->lighting());
    drawWizard(device, clip);
    m_enemyMissiles.draw(device, clip, m_world->lighting());
    m_missiles.draw(device, clip, m_world->lighting());
    m_effects.draw(device, clip, m_world->fullLighting());
    drawSpawn(device, clip);
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    // The welcome's cut is letterboxed the way the original's trigger cameras are: black
    // bars top and bottom, the status boxes hidden beneath the lower one.
    const bool cut = m_intro == Intro::Crystal;
    m_transition.draw(m_canvas, width); // over the view, under the boxes
    if (!cut) {
        for (s32 player = 0; player < kPlayerCount; ++player) {
            m_boxes.draw(m_canvas, player, statusOf(player), true);
        }
        m_pickups.draw(m_canvas, m_boxes);
        m_bossMeter.draw(m_canvas, device);
        drawCaption(width, height);
    }
    if (spawning()) {
        drawLevelTitle(width);
    }
    if (cut) {
        m_canvas.fill(Rect{0.0f, 0.0f, width, height * kCutBarTop}, Color::black());
        m_canvas.fill(Rect{0.0f, height * (1.0f - kCutBarBottom), width, height * kCutBarBottom},
                      Color::black());
    }
    if (!cut) {
        drawSelectors();
        drawHelp(clip, width, height);
    }
    m_scroll.draw(m_canvas);
    m_hintMenu.draw(m_canvas, m_text);
    m_canvas.end();
}

/** The help message, over the head of the character it is for. */
void PlayScene::drawHelp(const Mat4& clip, f32 width, f32 height) {
    if (!m_help.showing()) {
        return;
    }
    Vec2 head{width * 0.5f, height * 0.5f};
    for (const PlayerActor& actor : m_actors) {
        if (actor.player() != m_help.player()) {
            continue;
        }
        const Vec4 point = clip * Vec4{actor.followPoint(), 1.0f};
        if (point.w > 1e-4f) {
            head = Vec2{(point.x / point.w * 0.5f + 0.5f) * width,
                        (0.5f - point.y / point.w * 0.5f) * height};
        }
    }
    const auto sheet = m_staticTextures.loaded() ? m_staticTextures.find(kScrollTexture)
                                                 : std::nullopt;
    const Texture* scroll = nullptr;
    if (sheet.has_value() && m_device != nullptr) {
        try {
            scroll = &m_staticTextures.texture(*m_device, *sheet);
        } catch (const std::exception&) {
            scroll = nullptr;
        }
    }
    m_help.draw(m_canvas, m_boxes.smallCaps(), scroll, head); // the strings' own font
}

CameraView PlayScene::cameraView() const {
    CameraView view;
    if (m_context.config != nullptr) {
        view.horizontalFov = m_context.config->horizontalFovRadians();
        view.aspect = static_cast<f32>(m_context.config->display.frameWidth) /
                      static_cast<f32>(m_context.config->display.frameHeight);
    }
    return view;
}

const PlayerActor* PlayScene::actor(s32 player) const {
    for (const PlayerActor& actor : m_actors) {
        if (actor.player() == player) {
            return &actor;
        }
    }
    return nullptr;
}

const PlayerAnimator* PlayScene::animator(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player) {
            return m_figures[i] != nullptr && m_figures[i]->animator.bound()
                       ? &m_figures[i]->animator
                       : nullptr;
        }
    }
    return nullptr;
}

StatusBoxView PlayScene::statusOf(s32 player) const {
    StatusBoxView view;
    const PlayerActor* actor = this->actor(player);
    if (actor == nullptr) {
        return view;
    }
    const CharacterSave& save = actor->save();
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = save.character;
    view.color = save.color;
    view.name = save.name;
    view.level = experienceLevel(save.experience());
    view.gold = save.gold;
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player && isDown(i)) {
            view.inTower = m_down[i] == kInTower;
            view.health = 0;
            return view;
        }
    }
    view.health = save.health();
    if (const TurboMeter* meter = turboMeter(player); meter != nullptr) {
        view.turbo = meter->look();
    }
    view.keys = save.progress().inventory.keys;
    view.potions = static_cast<s32>(save.progress().inventory.potions.size());
    view.potionKind = save.progress().inventory.nextPotion();
    return view;
}

/** Stands the materialising effect at every character's feet and, with `ride`, the start
 * camera at the entrance marker to hold and ride in; the party holds still for it. (The
 * realm's entering sound belongs to the loading screen, not to this.) */
void PlayScene::beginSpawn(RenderDevice& device, bool ride) {
    m_spawns.clear();
    m_spawnTexmods.clear();
    m_spawnFrames = 0.0f;
    m_spawnTicks = kSpawnTicks;
    m_startCamera.stop();
    m_titleSlide = kTitleSlideStart;
    if (const auto marker = m_world->entranceCamera(); ride && marker.has_value()) {
        Vec3 centre{0.0f, 0.0f, 0.0f};
        for (const PlayerActor& actor : m_actors) {
            centre += actor.position();
        }
        if (!m_actors.empty()) {
            centre /= static_cast<f32>(m_actors.size());
        }
        m_startCamera.start(*marker, centre);
    } else if (ride) {
        log::warn("Tower: no start camera; the party appears under the follow camera");
    }
    if (!m_weapons.loaded()) {
        m_weapons.load(m_context.unpackedRoot / kWeaponsArchive);
    }
    const auto tree = m_weapons.loaded() ? m_weapons.trees.find(kSpawnEffect) : std::nullopt;
    if (!tree.has_value()) {
        log::warn("Tower: no {} in {}; the party appears without it", kSpawnEffect,
                  kWeaponsArchive);
    } else {
        const TreeInfo& effect = m_weapons.trees.tree(*tree);
        for (const PlayerActor& actor : m_actors) {
            Spawn spawn;
            spawn.position = actor.position();
            spawn.tree = &effect;
            if (!spawn.model.bind(effect, m_weapons.models, m_weapons.textures, device)) {
                continue;
            }
            if (!effect.sequences.empty()) {
                spawn.player.start(effect.sequences[0], 0);
                spawn.pose.evaluate(effect, 0, 0.0f);
                spawn.model.setFrame(0, 0);
            } else {
                spawn.pose.rest(effect);
            }
            m_spawns.push_back(std::move(spawn));
        }
        m_spawnTexmods.bind(m_weapons.trees.textureAnimations(), m_weapons.textures, device);
    }
}

void PlayScene::updateSpawn(s32 /*ticks*/, f32 seconds) {
    m_spawnFrames += seconds * AnimationPlayer::kDefaultRate;
    const f32 whole = std::floor(m_spawnFrames);
    m_spawnFrames -= whole;
    if (whole > 0.0f) {
        m_spawnTexmods.step(static_cast<u32>(whole));
    }
    for (Spawn& spawn : m_spawns) {
        if (spawn.player.playing() && !spawn.player.finished()) {
            spawn.player.advance(seconds, false);
            spawn.pose.evaluate(*spawn.tree, spawn.player.sequence(), spawn.player.frame());
            spawn.model.setFrame(spawn.player.sequence(),
                                 static_cast<s32>(spawn.player.frame()));
        }
        for (usize i = 0; i < m_spawnTexmods.size(); ++i) {
            const TextureMotion motion = m_spawnTexmods.motion(i);
            if (motion.frame != nullptr) {
                spawn.model.setTextureFrame(motion.slot, motion.frame);
            } else {
                spawn.model.setTextureOffset(motion.slot, motion.offset);
            }
        }
    }
}

void PlayScene::drawSpawn(RenderDevice& device, const Mat4& clip) const {
    if (m_spawnTicks <= 0) {
        return;
    }
    for (const Spawn& spawn : m_spawns) {
        spawn.model.draw(device, clip, glm::translate(Mat4{1.0f}, spawn.position),
                         m_world->lighting(), spawn.pose.matrices());
    }
}

/** The level's title across the top of the screen while the party materialises, sliding up
 * as the start camera holds, the way the original places it. */
void PlayScene::drawLevelTitle(f32 width) {
    const LevelInfo* level = m_world->level();
    if (level == nullptr || level->title.empty() || !m_text.ready()) {
        return;
    }
    const TextStyle style;
    const s32 y = kTitleY - static_cast<s32>(static_cast<f32>(kTitleLift) * m_titleSlide);
    m_text.draw(m_canvas, -static_cast<s32>(width / 2.0f), y, level->title, style);
}

/** Opens one page of a scroll message over the tower: the party reads it and presses on. */
bool PlayScene::openMessage(std::string_view name, usize page) {
    if (m_device == nullptr || !m_scrollText.loaded()) {
        return false;
    }
    const auto found = m_scrollText.find(name);
    if (!found.has_value() || page >= m_scrollText.message(*found).pages.size()) {
        log::warn("Tower: no page {} of the message {}", page, name);
        return false;
    }
    const MessageInfo& message = m_scrollText.message(*found);
    const std::string prompt =
        m_context.strings != nullptr ? std::string(m_context.strings->get(kPromptText)) : "";
    return m_scroll.open(*m_device, {message.pages[page]}, message.scale, prompt);
}

/** Plays a sound by name from whichever loaded bank holds it; false when none does. */
SoundHandle PlayScene::playNamed(std::string_view name) {
    if (m_context.sounds == nullptr || name.empty()) {
        return kNoSound;
    }
    for (SoundSet* bank : {&m_levelBank, &m_commonSounds, &m_ambientBank}) {
        if (const auto found = bank->find(name); found.has_value()) {
            try {
                return m_context.sounds->play(bank->sequence(*found), 1.0f,
                                              SoundCategory::Effects);
            } catch (const std::exception& e) {
                log::warn("Tower: sound {}: {}", name, e.what());
                return kNoSound;
            }
        }
    }
    return kNoSound;
}

void PlayScene::stopVoice() {
    if (m_context.sounds != nullptr && m_voice != kNoSound) {
        m_context.sounds->stop(m_voice);
    }
    m_voice = kNoSound;
}

void PlayScene::stopOpeningSounds() {
    if (m_context.sounds != nullptr) {
        for (const OpeningSound& sound : m_openingSounds) {
            m_context.sounds->stop(sound.handle);
        }
    }
    m_openingSounds.clear();
}

/** Congratulates the party once its crystals open a realm's gate: the scroll for the realm,
 * its announcing voice, and the save remembers so it is not said twice. */
void PlayScene::announceUnlock(s32 realm) {
    if (realm <= 0 || static_cast<usize>(realm) >= kRealmCount) {
        return;
    }
    const u32 bit = 1U << static_cast<u32>(realm);
    bool fresh = false;
    for (PlayerActor& actor : m_actors) {
        ClassProgress& progress = actor.save().progress();
        fresh = fresh || (progress.unlocked & bit) == 0;
        progress.unlocked |= bit;
    }
    if (!fresh) {
        return;
    }
    openMessage(kUnlockLevel, static_cast<usize>(realm));
    if (static_cast<usize>(realm) < kUnlockVoices.size()) {
        stopVoice();
        m_voice = playNamed(kUnlockVoices[static_cast<usize>(realm)]);
    }
}

/** Tells a refused party what a gate wants; a target opening before them (a gate's field, a
 * lift, a gate) sounds its slot's note until it is done, then the note of its end. */
void PlayScene::handleTriggerEvents() {
    // One scroll at a time: the frame's first refusal.
    if (const std::vector<TriggerRefusal> refusals = m_world->takeTriggerRefusals();
        !refusals.empty()) {
        const TriggerRefusal& refusal = refusals.front();
        if (refusal.crystals) {
            openMessage(kNeedCrystals, static_cast<usize>(refusal.id));
        } else if (const s32 tier = refusal.id - kIconTierBase; tier >= 0) {
            openMessage(kNeedIcons, static_cast<usize>(tier));
        }
    }
    for (const TriggerOpening& opening : m_world->takeTriggerOpenings()) {
        const OpeningSounds* sounds = openingSoundsOf(opening.sound);
        if (opening.atOnce || sounds == nullptr) {
            continue;
        }
        if (const SoundHandle handle = playNamed(sounds->moving); handle != kNoSound) {
            m_openingSounds.push_back(OpeningSound{opening.target, handle});
        }
    }
    for (const TriggerOpening& settled : m_world->takeTriggerSettled()) {
        for (usize i = 0; i < m_openingSounds.size();) {
            if (m_openingSounds[i].target == settled.target) {
                if (m_context.sounds != nullptr) {
                    m_context.sounds->stop(m_openingSounds[i].handle);
                }
                m_openingSounds.erase(m_openingSounds.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        if (const OpeningSounds* sounds = openingSoundsOf(settled.sound); sounds != nullptr) {
            playNamed(sounds->done);
        }
    }
}

} // namespace gdl::game
