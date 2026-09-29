#include "game/screens/PartyFigures.h"

#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/world/DynamicLights.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {

namespace {

constexpr std::string_view kItSign = "IT_SIGN"; ///< on the back of who is it
constexpr std::string_view kGauntletObject = "BOSSGAUNTL";
/** A player's shadow looks for its floor from this over the feet, and this far under them. */
constexpr f32 kShadowReach = 1.0f;
constexpr f32 kShadowDrop = 100.0f;

/** A texture of `archive` by name, or none (with a word in the log) when it is not there. */
const Texture* skinTexture(RenderDevice& device, ItemArchive& archive, std::string_view name) {
    const auto index = archive.textures.find(name);
    if (!index) {
        return nullptr;
    }
    try {
        return &archive.textures.texture(device, *index);
    } catch (const std::exception& e) {
        log::warn("Player skin {}: {}", name, e.what());
        return nullptr;
    }
}

} // namespace

void PartyFigures::loadSkins(RenderDevice& device, ItemArchive& powerups, ItemArchive& weapons) {
    m_hitFlash = skinTexture(device, powerups, kHitFlashSkin);
    m_goldSkin = skinTexture(device, weapons, kGoldSkin);
    m_silverSkin = skinTexture(device, weapons, kSilverSkin);
}

void PartyFigures::clear() {
    m_hitFlash = nullptr;
    m_goldSkin = nullptr;
    m_silverSkin = nullptr;
}

const Texture* PartyFigures::skinOf(const PlayerRuntime& runtime, const PowerupEffects& worn,
                                    const PortalDeparture& departure) const {
    if (departure.started()) {
        return departure.skin();
    }
    if (runtime.hitFlashTicks > 0) {
        return m_hitFlash;
    }
    switch (worn.chrome()) {
    case PowerupEffects::Chrome::Gold: return m_goldSkin;
    case PowerupEffects::Chrome::Silver: return m_silverSkin;
    case PowerupEffects::Chrome::None: break;
    }
    return departure.skin();
}

std::string_view PartyFigures::shieldObjectOf(u32 armor) {
    if ((armor & powerup::kReflectShield) != 0) {
        return "RF_SHLD";
    }
    if ((armor & powerup::kFireShield) != 0) {
        return "FW_SHLD";
    }
    if ((armor & powerup::kLightningShield) != 0) {
        return "L_SHLD";
    }
    return {};
}

bool PartyFigures::shown(const PlayerRuntime& runtime, const Scene& scene) {
    return runtime.figure != nullptr && runtime.life != PlayerLife::InTower &&
           !scene.departure.finished();
}

f32 PartyFigures::alphaOf(const PlayerRuntime& runtime, const PowerupEffects& worn) {
    return worn.bodyAlpha() * runtime.transport.alpha();
}

void PartyFigures::draw(RenderDevice& device, std::span<const PlayerRuntime> players,
                        const Scene& scene, const Mat4& clip, const CameraFrame& camera) const {
    LevelWorld& world = scene.world;
    for (const PlayerRuntime& runtime : players) {
        if (!shown(runtime, scene)) {
            continue;
        }
        PlayerFigure& figure = *runtime.figure;
        const PowerupEffects worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
        const f32 size = PlayerFigure::bodyScale(runtime.actor.save(), worn);
        const Mat4 body = glm::scale(
            scene.departure.transform(runtime.capture.body().value_or(runtime.actor.transform())),
            Vec3{size, size, size});
        figure.setSkinTexture(skinOf(runtime, worn, scene.departure));
        // On the second hand: the left gauntlet, else a shield (PlayerProcessPowerups).
        if ((worn.special & powerup::kLeftGauntlet) != 0) {
            figure.holdOnArm(device, &world.powerups(), kGauntletObject);
        } else {
            figure.holdOnArm(device, &scene.weapons, shieldObjectOf(worn.armor));
        }
        const f32 alpha = alphaOf(runtime, worn);
        figure.draw(device, clip, body, world.lighting(), alpha, runtime.move.weaponHidden(),
                    &camera);
        figure.drawHeadwear(device, world.powerups(), worn, clip, body, world.lighting(), alpha);
        figure.drawGem(device, world.powerups(), runtime.gem.shown(), clip, body, world.lighting(),
                       alpha);
        // Who is it wears the realm's sign on their back (player.c 5885).
        if (runtime.itTicks > 0) {
            figure.drawMarker(device, world.realmItems(), kItSign, clip, body, world.lighting(),
                              alpha);
        }
        figure.setSkinTexture(nullptr);
    }
}

void PartyFigures::greetGems(RenderDevice& device, std::span<PlayerRuntime> players,
                             ItemArchive& powerups, EffectTrees& effects) {
    for (PlayerRuntime& runtime : players) {
        const u32 special = PowerupEffects::of(runtime.actor.save().progress().inventory).special;
        if (runtime.gem.update(special) && powerups.loaded()) {
            effects.start(device, powerups, HeadGem::kAppearEffect, runtime.actor.followPoint());
        }
    }
}

void PartyFigures::drawShadows(RenderDevice& device, std::span<const PlayerRuntime> players,
                               const Scene& scene, const Mat4& clip, const Vec3& eye) {
    if (!scene.world.ref().playerShadows()) {
        return;
    }
    for (const PlayerRuntime& runtime : players) {
        if (!shown(runtime, scene)) {
            continue;
        }
        // It lies on the floor under the body, even while the body is thrown or sinks
        // (PlayerMotion keeps its height at the floor, not the feet).
        const Vec3 feet{scene.departure.transform(
            runtime.capture.body().value_or(runtime.actor.transform()))[3]};
        if (const auto floor = scene.world.collision().floorAt(feet, kShadowReach, kShadowDrop)) {
            const PowerupEffects worn =
                PowerupEffects::of(runtime.actor.save().progress().inventory);
            runtime.figure->drawShadow(device, clip, eye, Vec3{feet.x, floor->y, feet.z},
                                       floor->normal, scene.world.lighting(),
                                       alphaOf(runtime, worn));
        }
    }
}

void PartyFigures::addLanterns(std::vector<PointLight>& lights,
                               std::span<const PlayerRuntime> players, const LevelInfo* level) {
    if (level == nullptr || (level->flags & DynamicLights::kDarkLevel) == 0) {
        return;
    }
    for (const PlayerRuntime& runtime : players) {
        if (runtime.life != PlayerLife::Standing) {
            continue;
        }
        PointLight lantern;
        lantern.position =
            runtime.actor.followPoint() + Vec3{0.0f, DynamicLights::kLanternLift, 0.0f};
        lantern.color = DynamicLights::lantern(runtime.actor.save().color);
        lantern.radius = DynamicLights::kLanternRadius;
        lantern.intensity = DynamicLights::kLanternIntensity;
        lights.push_back(lantern);
    }
}

} // namespace gdl::game
