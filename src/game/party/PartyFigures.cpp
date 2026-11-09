#include "game/party/PartyFigures.h"

#include <cmath>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/DynamicLights.h"
#include "game/party/PlayerFigure.h"

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

/** Only standing survivors depart; a teammate can still be dying. Imported
 * levels may replace GDL's sinking spin without moving gameplay/collision. */
Mat4 departurePlacement(const PlayerRuntime& runtime, const PortalDeparture& departure,
                        f32 frameBlend, const ImportedLevel* imported) {
    const auto native = imported ? imported->departure(frameBlend) : std::nullopt;
    const Mat4 body = runtime.capture.body().value_or(
        PartyFigures::presentationBody(runtime, departure.started() || native ? 1.0f : frameBlend));
    if (native && runtime.life == PlayerLife::Standing) {
        return glm::scale(portable::rotate(body, native->yaw, Vec3{0, 1, 0}), Vec3{native->scale});
    }
    return runtime.life == PlayerLife::Standing ? departure.transform(body) : body;
}

} // namespace

void PartyFigures::snapshot(std::span<PlayerRuntime> players) {
    for (auto& runtime : players) {
        if (runtime.figure) {
            runtime.figure->capturePresentation();
        }
        runtime.previous = {runtime.actor.position(), runtime.actor.yaw(),
                            !runtime.capture.body().has_value() && !runtime.transport.active(),
                            runtime.figure ? runtime.figure->animationRevision() : 0};
    }
}

f32 PartyFigures::presentationBlend(const PlayerRuntime& runtime, f32 frameBlend) {
    // Scrolls and cuts can update the scene while deliberately holding its figures.
    // Do not cycle their last two animated poses as the render clock keeps running.
    return runtime.figure &&
                   runtime.figure->animationRevision() != runtime.previous.animationRevision
               ? frameBlend
               : 1.0f;
}

bool PartyFigures::presentationContinuous(const PlayerRuntime& runtime) {
    const PlayerActor& actor = runtime.actor;
    // A relocation is not movement to draw through. Ordinary motion per fixed tick is
    // much smaller than the body's collision diameter, including on moving platforms.
    const Vec3 displacement = actor.position() - runtime.previous.position;
    const f32 diameter = actor.radius() * 2;
    return runtime.previous.continuous && !runtime.transport.active() &&
           glm::dot(displacement, displacement) <= diameter * diameter;
}

Mat4 PartyFigures::presentationBody(const PlayerRuntime& runtime, f32 frameBlend) {
    const PlayerActor& actor = runtime.actor;
    if (!presentationContinuous(runtime)) {
        return actor.transform();
    }
    const f32 t = std::clamp(frameBlend, 0.0f, 1.0f);
    const Vec3 position = glm::mix(runtime.previous.position, actor.position(), t);
    const f32 yaw =
        runtime.previous.yaw + std::remainder(actor.yaw() - runtime.previous.yaw, kTwoPi) * t;
    return portable::rotate(glm::translate(Mat4{1}, position), yaw, Vec3{0, 1, 0});
}

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
                                    const PortalDeparture& departure,
                                    const ImportedLevel* imported) const {
    if (runtime.life == PlayerLife::Standing && imported) {
        if (const auto native = imported->departure()) {
            return native->skin;
        }
    }
    if (runtime.life == PlayerLife::Standing && departure.started()) {
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
    return nullptr;
}

std::string_view PartyFigures::shieldObjectOf(u32 armor) {
    if ((armor & armor::kReflect) != 0) {
        return "RF_SHLD";
    }
    if ((armor & armor::kFireArmor) != 0) {
        return "FW_SHLD";
    }
    if ((armor & armor::kElectricArmor) != 0) {
        return "L_SHLD";
    }
    return {};
}

bool PartyFigures::shown(const PlayerRuntime& runtime, const Scene& scene) {
    const auto* imported = scene.world.imported();
    const auto native = imported ? imported->departure(scene.frameBlend) : std::nullopt;
    const bool finished =
        native ? native->finished && !native->keepVisible : scene.departure.finished();
    return runtime.figure != nullptr && runtime.life != PlayerLife::InTower &&
           (runtime.life != PlayerLife::Standing || !finished);
}

f32 PartyFigures::alphaOf(const PlayerRuntime& runtime, const PowerupEffects& worn) {
    return worn.bodyAlpha() * runtime.transport.alpha();
}

void PartyFigures::draw(RenderDevice& device, std::span<const PlayerRuntime> players,
                        const Scene& scene, const Mat4& clip, const CameraFrame& camera,
                        Bodies which) const {
    LevelWorld& world = scene.world;
    for (const PlayerRuntime& runtime : players) {
        if (runtime.mikeyFigure != nullptr && which == Bodies::Solid) {
            runtime.mikeyFigure->draw(device, clip, world.lighting(), &camera);
        }
        if (!shown(runtime, scene)) {
            continue;
        }
        PlayerFigure& figure = *runtime.figure;
        const PowerupEffects worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
        const f32 alpha = alphaOf(runtime, worn);
        if ((alpha < 1.0f) != (which == Bodies::Translucent)) {
            continue;
        }
        const Mat4 body = figure.placement(
            departurePlacement(runtime, scene.departure, scene.frameBlend, world.imported()),
            runtime.actor.save(), worn);
        const auto native =
            world.imported() ? world.imported()->departure(scene.frameBlend) : std::nullopt;
        figure.setSkinTexture(skinOf(runtime, worn, scene.departure, world.imported()),
                              native ? native->skinAlpha : TreeModel::SkinAlpha::PreserveCoverage);
        figure.setWeaponPowerups(device, world.powerups(), scene.weapons, worn);
        // On the second hand: the left gauntlet, else a shield (PlayerProcessPowerups).
        if ((worn.special & powerup::kLeftGauntlet) != 0) {
            figure.holdOnArm(device, &world.powerups(), kGauntletObject);
        } else {
            figure.holdOnArm(device, &scene.weapons, shieldObjectOf(worn.armor));
        }
        const WorldLighting lighting = runtime.glow.apply(world.lighting());
        // An unseen body is drawn after the generators' glows but before the level's sorted
        // scenery; writing depth keeps that scenery behind it (the Spider's floorboards).
        figure.setFadedDepthWrite(which == Bodies::Translucent);
        figure.draw(device, clip, body, lighting, alpha, runtime.move.weaponHidden(), &camera,
                    presentationBlend(runtime, scene.frameBlend),
                    runtime.actor.player() == scene.occupiedHand, TreeModel::Pass::DepthWriting);
        figure.drawHeadwear(device, world.powerups(), worn, clip, body, lighting, alpha);
        figure.drawGem(device, world.powerups(), runtime.gem.shown(), clip, body, lighting, alpha);
        // Who is it wears the realm's sign on their back (player.c 5885).
        if (runtime.itTicks > 0) {
            figure.drawMarker(device, world.realmItems(), kItSign, clip, body, lighting, alpha);
        }
        figure.setSkinTexture(nullptr);
    }
}

void PartyFigures::drawEffects(RenderDevice& device, std::span<const PlayerRuntime> players,
                               const Scene& scene, const Mat4& clip, const CameraFrame& camera) {
    for (const PlayerRuntime& runtime : players) {
        if (!shown(runtime, scene)) {
            continue;
        }
        const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
        const Mat4 body = runtime.figure->placement(
            departurePlacement(runtime, scene.departure, scene.frameBlend, scene.world.imported()),
            runtime.actor.save(), worn);
        runtime.figure->drawCompanions(
            device, clip, body, runtime.glow.apply(scene.world.lighting()), alphaOf(runtime, worn),
            &camera, presentationBlend(runtime, scene.frameBlend), TreeModel::Pass::Effects);
    }
}

void PartyFigures::updateDecoys(RenderDevice& device, std::span<PlayerRuntime> players,
                                ItemArchive& powerups, f32 seconds, EffectTrees& effects) {
    for (PlayerRuntime& runtime : players) {
        if (runtime.mikey.shown() && runtime.mikeyFigure == nullptr) {
            runtime.mikeyFigure = std::make_unique<MikeyFigure>();
        }
        if (runtime.mikeyFigure != nullptr) {
            runtime.mikeyFigure->update(device, powerups, runtime.mikey, seconds, effects);
        }
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
    for (const PlayerRuntime& runtime : players) {
        if (const auto shadow = shadowOf(runtime, scene.world, scene.departure, scene.frameBlend)) {
            runtime.figure->drawShadow(device, clip, eye, shadow->ground, shadow->normal,
                                       scene.world.lighting(), shadow->alpha);
        }
    }
}

std::optional<PartyFigures::Shadow> PartyFigures::shadowOf(const PlayerRuntime& runtime,
                                                           const LevelWorld& world,
                                                           const PortalDeparture& departure,
                                                           f32 frameBlend) {
    const auto* imported = world.imported();
    const auto native = imported ? imported->departure(frameBlend) : std::nullopt;
    const bool finished = native ? native->finished : departure.finished();
    if (!world.ref().playerShadows() || runtime.figure == nullptr ||
        runtime.life == PlayerLife::InTower || (runtime.life == PlayerLife::Standing && finished)) {
        return std::nullopt;
    }
    // Probe the actual host floor, including moving platforms and slopes, even while
    // the body is thrown or sinking. A guest's unloaded trigger state cannot supply it.
    const Vec3 feet{departurePlacement(runtime, departure, frameBlend, imported)[3]};
    if (const auto floor = world.collision().floorAt(feet, kShadowReach, kShadowDrop)) {
        const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
        return Shadow{{feet.x, floor->y, feet.z}, floor->normal, alphaOf(runtime, worn)};
    }
    return std::nullopt;
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
