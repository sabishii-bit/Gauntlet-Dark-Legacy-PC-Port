#include "game/scenes/TowerCeremonies.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <vector>

#include "game/characters/ClassData.h"
#include "game/effects/EffectTrees.h"
#include "game/level/ExitPortals.h"
#include "game/level/LevelWorld.h"
#include "game/level/TowerAccess.h"
#include "game/players/PickupVoices.h"
#include "game/players/PlayerAnimator.h"
#include "game/tower/TowerCompletion.h"

namespace gdl::game {

void TowerCeremonies::open(const Scene& scene, const MessageTable& hudStrings,
                           const MessageTable& scrollStrings, ExitPortals& portals) {
    m_promotion.begin(scene.players, hudStrings);
    m_promotion.bind(scene.device, scene.world.items(), scene.world.layout(), scene.players);
    std::vector<Relics> collection;
    Vec3 centre{0};
    for (const auto& runtime : scene.players) {
        collection.push_back(runtime.actor.save().progress().relics);
        centre += runtime.actor.position();
    }
    if (!scene.players.empty()) {
        centre /= static_cast<f32>(scene.players.size());
    }
    m_relics.begin(collection, scrollStrings);
    m_relics.bind(scene.device, scene.world, centre);
    // If another party member has already installed a piece, nobody needs to
    // replay it. The acquisition itself remains in each character's save.
    for (auto& runtime : scene.players) {
        auto& relics = runtime.actor.save().progress().relics;
        relics.pendingRunes &= static_cast<u16>(~m_relics.displayedRunes());
        relics.pendingShards &= static_cast<u16>(~m_relics.displayedShards());
        relics.pendingCeremonies = m_relics.pendingCeremonies();
        // The temple boss has no window piece. Its return can instead complete
        // the Underworld route, now retained by the follow-up's pending bit.
        relics.pendingShards &= static_cast<u16>(~(1U << TowerAccess::kTempleShardOrder));
    }
    revealRoutes(scene.world, portals);
}

void TowerCeremonies::clearCeremonies() {
    m_promotion.clear();
    m_relics.clear();
    m_relicVoice = {};
    m_promotionVoice = {};
}

void TowerCeremonies::releaseFigures() {
    m_bindings.clear();
    m_retainedFigures.clear();
}

void TowerCeremonies::advanceSpeech(f32 seconds) {
    m_promotionVoice.timing.advance(seconds);
    m_relicVoice.timing.advance(seconds);
}

void TowerCeremonies::revealRoutes(LevelWorld& world, ExitPortals& portals) {
    portals.syncFloors();
    for (const auto kind : {TowerCompletion::Kind::Window, TowerCompletion::Kind::Underworld,
                            TowerCompletion::Kind::Garm}) {
        const auto tag = TowerCompletion::portal(kind);
        const f32 alpha = m_relics.revealAlpha(kind);
        portals.setAlpha(tag, alpha);
        const auto glow = TowerAccess::glowObjectName(TowerAccess::worldOfLetter(tag[0]),
                                                      ExitPortals::gateOf(tag));
        for (usize i = 0; i < portals.size(); ++i) {
            if (portals.portal(i).tag != tag || portals.portal(i).shut) {
                continue;
            }
            const auto& objects = world.layout().objects();
            for (usize object = 0; object < objects.size(); ++object) {
                if (objects[object].name == glow) {
                    world.setObjectAlpha(object, alpha);
                }
            }
        }
    }
}

void TowerCeremonies::advanceRelics(const Scene& scene, ExitPortals& portals, s32 ticks,
                                    f32 seconds) {
    const auto cue = m_relics.update(ticks, seconds, m_relicVoice.timing.playing());
    if (!cue.voice.empty()) {
        m_relicVoice = scene.audio.speechPromotion(cue.voice);
    }
    if (!cue.sound.empty()) {
        scene.audio.playPromotion(cue.sound);
    }
    if (cue.completed) {
        for (auto& runtime : scene.players) {
            TowerRelics::acknowledge(runtime.actor.save().progress().relics, *cue.completed);
        }
    }
    revealRoutes(scene.world, portals);
}

bool TowerCeremonies::ownsFigure(std::span<const PlayerRuntime> players, usize player,
                                 const PlayerFigure* figure) const {
    return player < players.size() &&
           (figure == nullptr || players[player].figure.get() == figure ||
            std::ranges::any_of(m_retainedFigures, [figure](const auto& retained) {
                return retained.get() == figure;
            }));
}

void TowerCeremonies::activateFigure(std::span<PlayerRuntime> players, usize player,
                                     PlayerFigure* figure) {
    auto& active = players[player].figure;
    if (active.get() == figure) {
        return;
    }
    if (figure == nullptr) {
        m_retainedFigures.push_back(std::move(active));
        return;
    }
    for (auto& retained : m_retainedFigures) {
        if (retained.get() == figure) {
            active.swap(retained);
            return;
        }
    }
}

void TowerCeremonies::upgradeFigure(const Scene& scene, usize player) {
    auto& runtime = scene.players[player];
    const auto& save = runtime.actor.save();
    const s32 level = save.progress().appearanceLevel();
    const auto cached = std::ranges::find_if(m_bindings, [&](const auto& binding) {
        return binding.player == player && binding.character == save.character &&
               binding.color == save.color && binding.level == level;
    });
    if (cached != m_bindings.end()) {
        // A discarded promotion keeps its resources alive: replay restores the
        // new costume's birth values rather than loading a second copy of it.
        activateFigure(scene.players, player, cached->figure);
        runtime.figure->restoreLocal(cached->initial);
    } else {
        auto figure = PlayerFigure::load(scene.device, scene.unpackedRoot, save, Entrance::Skip);
        if (figure == nullptr) {
            return;
        }
        PromotionFigure binding;
        binding.player = player;
        binding.character = save.character;
        binding.color = save.color;
        binding.level = level;
        binding.figure = figure.get();
        figure->saveLocal(binding.initial);
        m_bindings.push_back(std::move(binding));
        m_retainedFigures.push_back(std::move(runtime.figure));
        runtime.figure = std::move(figure);
    }
    runtime.figure->setCompanionPowerups(scene.device, scene.world.powerups(),
                                         save.progress().inventory, &scene.weapons);
}

void TowerCeremonies::advancePromotion(const Scene& scene, s32 ticks) {
    const auto* entry = m_promotion.current();
    if (entry == nullptr) {
        return;
    }
    auto& runtime = scene.players[entry->player];
    auto& save = runtime.actor.save();
    const auto cue = m_promotion.update(ticks, m_promotionVoice.timing.playing());
    if (cue.voice) {
        LevelSoundscape::Speech name;
        if (PickupVoices::carriesPojo(save)) {
            name = scene.audio.speechNarration(LevelSoundscape::kPojoName);
        } else if (runtime.figure != nullptr) {
            name = scene.audio.speechFrom(runtime.figure->voice(),
                                          PickupVoices::nameOf(save.character, save.color));
        }
        m_promotionVoice = scene.audio.speechPromotion(entry->voice, name);
    }
    if (cue.award) {
        save.progress().promotedLevel = entry->level;
        upgradeFigure(scene, entry->player);
        EffectTrees::Setting setting;
        setting.seconds = 3;
        scene.effects.startSet(scene.device, scene.weapons,
                               std::format("LEVELUP_{}", colorCode(save.color)),
                               runtime.actor.position(), setting);
    }
    if (cue.gem) {
        constexpr std::array<std::string_view, 4> kGems{"GETGEMYELLOW", "GETGEMBLUE", "GETGEMRED",
                                                        "GETGEMGREEN"};
        scene.effects.start(scene.device, scene.world.powerups(),
                            kGems[static_cast<usize>(save.color)], runtime.actor.position());
    }
}

void TowerCeremonies::saveLocal(TowerPromotion::LocalSnapshot& promotion,
                                TowerRelics::LocalSnapshot& relics, SpeechTiming& relicSpeech,
                                SpeechTiming& promotionSpeech, SoundCueToken& relicCue,
                                SoundCueToken& promotionCue) const {
    m_promotion.saveLocal(promotion);
    m_relics.saveLocal(relics);
    relicSpeech = m_relicVoice.timing;
    promotionSpeech = m_promotionVoice.timing;
    relicCue = m_relicVoice.cue;
    promotionCue = m_promotionVoice.cue;
}

bool TowerCeremonies::canRestoreLocal(const TowerPromotion::LocalSnapshot& promotion,
                                      const TowerRelics::LocalSnapshot& relics) const {
    return m_promotion.canRestoreLocal(promotion) && m_relics.canRestoreLocal(relics);
}

void TowerCeremonies::restoreLocal(const TowerPromotion::LocalSnapshot& promotion,
                                   const TowerRelics::LocalSnapshot& relics,
                                   const SpeechTiming& relicSpeech,
                                   const SpeechTiming& promotionSpeech,
                                   const SoundCueToken& relicCue,
                                   const SoundCueToken& promotionCue) {
    m_promotion.restoreLocal(promotion);
    m_relics.restoreLocal(relics);
    m_relicVoice.timing = relicSpeech;
    m_promotionVoice.timing = promotionSpeech;
    m_relicVoice.cue = relicCue;
    m_promotionVoice.cue = promotionCue;
}

bool TowerCeremonies::registerRetainedFigures(NativeStateResources& resources) const {
    // Corrected costume upgrades retain old figures so saved effects can still
    // borrow their class banks. Visit the actual owners: the initial costume is
    // not a promotion-cache entry. Register without activating or loading them.
    return std::ranges::all_of(m_retainedFigures, [&](const auto& figure) {
        return figure == nullptr || figure->registerNativeResources(resources);
    });
}

} // namespace gdl::game
