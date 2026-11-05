#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/render/ImmediateBatch.h"

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Gate(RenderDevice& device, formats::LegendsN64Gate definition,
                                      usize idleFigure) {
    definition.position = m_fixtures[idleFigure].position();
    ItemInstance placement;
    placement.position = definition.position;
    placement.rotation.y = definition.yaw;
    const usize opening = m_fixtures.size();
    place(device, formats::legendsN64TreeName("TRAP", definition.openingSequence), placement,
          definition.scale);
    if (m_fixtures.size() == opening) {
        m_warnings.push_back("Native locked door opening artwork is unavailable: " +
                             definition.openingSequence);
        return;
    }
    m_hiddenFixtures[idleFigure] = (definition.placementFlags & 8U) != 0;
    m_hiddenFixtures[opening] = true;
    m_gates.push_back({LegendsGate{std::move(definition)}, idleFigure, opening});
}

void LegendsSceneAdapter::touchGates(PlayerActor& actor) {
    const LegendsPortalVisitor visitor{static_cast<u8>(actor.player()), actor.position(), true,
                                       actor.radius(), actor.height()};
    for (auto& gate : m_gates) {
        auto& keys = actor.save().progress().inventory.keys;
        if (gate.state.touch(visitor, keys)) {
            if (gate.state.definition().sound != 0) {
                m_soundEvents.push_back(
                    {gate.state.definition().sound, gate.state.definition().position});
            }
            m_hiddenFixtures[gate.idleFigure] = true;
            m_hiddenFixtures[gate.openingFigure] =
                (gate.state.definition().placementFlags & 8U) != 0;
            m_fixtures[gate.openingFigure].play(0, ItemFigure::Playback::Once);
            m_fixtures[gate.openingFigure].capturePresentation();
        }
    }
}

void LegendsSceneAdapter::updateGates() {
    for (auto& gate : m_gates) {
        if (gate.state.activateInitially()) {
            if (gate.state.definition().sound != 0) {
                m_soundEvents.push_back(
                    {gate.state.definition().sound, gate.state.definition().position});
            }
            m_hiddenFixtures[gate.idleFigure] = true;
            m_hiddenFixtures[gate.openingFigure] =
                (gate.state.definition().placementFlags & 8U) != 0;
            m_fixtures[gate.openingFigure].play(0, ItemFigure::Playback::Once);
            m_fixtures[gate.openingFigure].capturePresentation();
        }
        if (gate.state.state() == LegendsGate::State::Opening &&
            m_fixtures[gate.openingFigure].finished()) {
            gate.state.finishOpening();
        }
    }
}

void LegendsSceneAdapter::bindN64Obelisk(RenderDevice& device,
                                         const formats::LegendsN64Obelisk& definition,
                                         usize idleFigure) {
    auto pickup = legendsN64ObeliskPickup(definition, nullptr, m_playerCount);
    if (!pickup) {
        return;
    }
    pickup->position = m_fixtures[idleFigure].position();
    ItemInstance placement;
    placement.position = pickup->position;
    placement.rotation.y = definition.yaw;
    const usize spent = m_fixtures.size();
    place(device, formats::legendsN64TreeName("TRAP", definition.spentSequence), placement,
          definition.scale);
    if (m_fixtures.size() == spent) {
        m_warnings.push_back("Native obelisk spent artwork is unavailable: " +
                             definition.spentSequence);
        return;
    }
    m_hiddenFixtures[spent] = true;
    m_obelisks.push_back(
        {m_pickups.size(), idleFigure, spent, {}, definition.sound, definition.identity});
    m_pickupFigures.push_back(idleFigure);
    m_pickups.push_back(std::move(*pickup));
}

void LegendsSceneAdapter::bindN64ObeliskEffects(RenderDevice& device,
                                                const std::filesystem::path& root) {
    // e00759ac resolves PTL_SPARKLE1 in the resident static archive. It is not
    // present in the stage/item banks. The emitter's private colour combiner
    // leaves the original texture intact for every other native consumer.
    const AssetLocator files{root / "static"};
    if (const auto objects = files.find("objects.wad")) {
        const auto textures = formats::decodeLegendsN64Textures(readFile(*objects));
        const auto found =
            std::ranges::find(textures, "PTL_SPARKLE1", &formats::LegendsN64Texture::name);
        if (found != textures.end() && !found->external && !found->image.pixels.empty()) {
            const auto image = legendsObeliskSparkle(found->image);
            TextureDesc description;
            description.width = image.width;
            description.height = image.height;
            description.wrap = TextureWrap::ClampToEdge;
            m_obeliskSparkle = device.createTexture(description, image.pixels);
        }
    }
    if (!m_obeliskSparkle) {
        m_warnings.push_back("Native obelisk particle texture static/PTL_SPARKLE1 is unavailable");
    }
}

void LegendsSceneAdapter::updateObelisks(f32 seconds) {
    for (auto& obelisk : m_obelisks) {
        const auto& pickup = m_pickups[obelisk.pickup];
        if (!pickup.collected) {
            continue;
        }
        if (!obelisk.motion.started()) {
            if (!m_progressInitialized) {
                obelisk.motion.restoreSpent();
            } else if (obelisk.motion.activate(pickup.position, pickup.nativeIndex)) {
                if (obelisk.sound != 0) {
                    m_soundEvents.push_back({obelisk.sound, pickup.position});
                }
                // e0068000 awards the party and requests the identity's advice.
                // The audio adapter resolves that advice from the native table.
                m_obeliskSpeechEvents.push_back(obelisk.identity);
            }
        }
        obelisk.motion.advance(seconds);
        const bool spent = obelisk.motion.spentModel();
        if (m_hiddenFixtures[obelisk.spentFigure] && spent) {
            auto& figure = m_fixtures[obelisk.spentFigure];
            const auto* sequence = figure.sequenceInfo(0);
            figure.play(0, sequence != nullptr && sequence->repeats ? ItemFigure::Playback::Loop
                                                                    : ItemFigure::Playback::Once);
            figure.capturePresentation();
        }
        m_hiddenFixtures[obelisk.idleFigure] = spent;
        m_hiddenFixtures[obelisk.spentFigure] = !spent;
    }
}

std::vector<u32> LegendsSceneAdapter::takeObeliskSpeech() {
    return std::exchange(m_obeliskSpeechEvents, {});
}

f32 LegendsSceneAdapter::obeliskWhiteBlend(usize figure) const {
    for (const auto& obelisk : m_obelisks) {
        if (figure == obelisk.idleFigure || figure == obelisk.spentFigure) {
            return obelisk.motion.whiteBlend();
        }
    }
    return 0;
}

void LegendsSceneAdapter::drawObeliskEffects(RenderDevice& device, const Mat4& clip,
                                             const CameraFrame& camera) const {
    if (!m_obeliskSparkle) {
        return;
    }
    ImmediateBatch batch;
    batch.begin(PrimitiveTopology::TriangleList);
    const Vec2 uv{-0.5f / static_cast<f32>(m_obeliskSparkle->width()),
                  -0.5f / static_cast<f32>(m_obeliskSparkle->height())};
    // e00759ac authors width 2.5, but e002180c transforms its billboard
    // offsets by the view basis that e00169b0 scales by 0.25. CameraFrame
    // supplies unit axes, so retain that native scale here, not on the
    // particle centres or the emitter's orbit/rise.
    constexpr f32 kSpriteWidth = 2.5f * .25f;
    for (const auto& obelisk : m_obelisks) {
        // e002180c billboard type0 uses a triangle covering a clamped sprite,
        // not a guessed quad or a reused GameCube particle descriptor.
        const f32 angle = obelisk.motion.spriteAngle();
        const Vec3 right = camera.right * std::cos(angle) + camera.up * std::sin(angle);
        const Vec3 up = camera.up * std::cos(angle) - camera.right * std::sin(angle);
        const Vec3 corner = (right + up) * (-.5f * kSpriteWidth);
        for (const auto& particle : obelisk.motion.particles()) {
            if (!particle.alive) {
                continue;
            }
            const Vec3 at = particle.position() + corner;
            const Color color = Color::white().withAlpha(particle.alpha);
            batch.vertex(at, color, uv);
            batch.vertex(at + up * (2 * kSpriteWidth), color, uv + Vec2{0, 2});
            batch.vertex(at + right * (2 * kSpriteWidth), color, uv + Vec2{2, 0});
        }
    }
    batch.end();
    if (!batch.empty()) {
        DrawState state;
        state.blend = BlendMode::Alpha;
        state.depthWrite = false;
        state.clampTexture = true;
        device.draw(batch, *m_obeliskSparkle, clip, state);
    }
}

void LegendsSceneAdapter::synchronizeProgress(std::span<const CharacterSave* const> party) {
    LegendsProgress combined;
    for (const auto* save : party) {
        if (!save || save->character < 0 || save->character >= kClassCount) {
            continue;
        }
        const auto& progress = save->progress().legendsProgress(m_platform);
        combined.runestones |= progress.runestones;
        combined.shards |= progress.shards;
        combined.obelisks |= progress.obelisks;
        combined.legendItems |= progress.legendItems;
        // The native gate accepts each earlier stage from ANY party member.
        // Keep only the verified policy roster in this temporary union, so four
        // full save histories cannot overflow the bounded per-character record.
        for (const auto& stage : m_stageUnlocks.stages()) {
            if (progress.hasCompleted(stage.map)) {
                combined.recordCompleted(stage.map);
            }
        }
    }
    // Native e00694e4 initializes an obelisk as spent if any active character
    // owns its bit. Later contacts award the whole party in one transaction.
    if (!m_progressInitialized) {
        for (auto& obelisk : m_obelisks) {
            auto& pickup = m_pickups[obelisk.pickup];
            if (pickup.award && combined.has(pickup.award->kind, pickup.award->index)) {
                pickup.collected = true;
            }
        }
    }
    m_partyProgress = std::move(combined);
    if (m_platform == LegendsPlatform::N64 && m_gatedPortals) {
        for (usize i = 0; i < m_portals.size(); ++i) {
            auto& portal = m_portalPresentation[i];
            const bool enabled = m_stageUnlocks.allows(m_portals[i].map, m_partyProgress);
            if (portal.enabled != enabled) {
                portal.enabled = enabled;
                portal.motion.reset();
                selectPortal(i);
            }
        }
    }
    updateObelisks();
    updateTowerProgress(!m_progressInitialized);
    m_progressInitialized = true;
}

void LegendsSceneAdapter::updateTowerProgress(bool initial) {
    const auto progress = LegendsTowerProgress::of(m_partyProgress);
    for (auto& door : m_towerDoors) {
        const u8 required = door.definition.requiredObelisks;
        if (required == 0 || (m_partyProgress.obelisks & required) != required ||
            door.motion.state() != LegendsTowerDoorMotion::State::Closed) {
            continue;
        }
        if (initial && door.openFigure != door.openingFigure) {
            door.motion = LegendsTowerDoorMotion{true};
            m_hiddenFixtures[door.openingFigure] = true;
            m_hiddenFixtures[door.openFigure] = false;
        } else if (door.motion.unlock()) {
            m_heldFixtures[door.openingFigure] = false;
            m_fixtures[door.openingFigure].play(0, ItemFigure::Playback::Once);
            m_fixtures[door.openingFigure].capturePresentation();
            if (!initial) {
                // Tower overlay e0865c98 case7 releases the door's held
                // animation and calls e007a13c(0x10) exactly once.
                m_soundEvents.push_back({0x10, door.definition.triggerPosition});
            }
        }
    }
    for (usize i = 0; i < m_towerRunestones.size(); ++i) {
        if (m_towerRunestones[i]) {
            m_hiddenFixtures[*m_towerRunestones[i]] = (progress.runestones & (1U << i)) == 0;
        }
    }
    for (usize i = 0; i < m_towerShards.size(); ++i) {
        if (m_towerShards[i]) {
            m_hiddenFixtures[*m_towerShards[i]] = (progress.shards & (1U << i)) == 0;
            m_fixtureOpacity[*m_towerShards[i]] = progress.shardOpacity();
        }
    }
    updateTowerPortals();
    if (m_cathedralBeam) {
        m_hiddenWorldObjects[*m_cathedralBeam] = !progress.cathedralBeam;
    }
}

} // namespace gdl::game
