#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/render/ImmediateBatch.h"

#include "expansions/n64/level/LegendsGates.h"
#include "expansions/n64/level/LegendsTowerProgress.h"
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
    m_gates.push_back({LegendsGate{legendsN64Gate(definition)}, idleFigure, opening});
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
        m_warnings.emplace_back(
            "Native obelisk particle texture static/PTL_SPARKLE1 is unavailable");
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

void LegendsSceneAdapter::updateTowerProgress(bool initial) {
    // The visit's ceremonies decide which pieces stand and when a completion shows; pieces
    // the party gains later show at once. Only the pieces are copied, not the party's records.
    const auto displayed = m_towerCeremony ? m_towerCeremony->displayed(m_partyProgress)
                                           : LegendsTowerCeremony::pieces(m_partyProgress);
    const auto progress = LegendsTowerProgress::of(displayed);
    const auto completion =
        m_towerCeremony ? LegendsTowerProgress::of(m_towerCeremony->completed(m_partyProgress))
                        : progress;
    for (auto& door : m_towerDoors) {
        const u8 required = door.definition.requiredObelisks;
        if (required == 0 || (displayed.obelisks & required) != required ||
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
            const auto* entry = m_towerCeremony ? m_towerCeremony->current() : nullptr;
            const bool flying = m_towerCeremony && m_towerCeremony->active() && entry != nullptr &&
                                entry->kind == LegendsTowerCeremony::Kind::Rune &&
                                entry->index == i && m_towerCeremony->runeInFlight();
            m_hiddenFixtures[*m_towerRunestones[i]] =
                !flying && (progress.runestones & (1U << i)) == 0;
        }
    }
    for (usize i = 0; i < m_towerShards.size(); ++i) {
        if (m_towerShards[i]) {
            m_hiddenFixtures[*m_towerShards[i]] = (progress.shards & (1U << i)) == 0;
            m_fixtureOpacity[*m_towerShards[i]] = completion.shardOpacity();
            if (m_towerCeremony && m_towerCeremony->active()) {
                const auto* entry = m_towerCeremony->current();
                if (entry && entry->kind == LegendsTowerCeremony::Kind::Shard) {
                    m_fixtureOpacity[*m_towerShards[i]] =
                        entry->index == i ? m_towerCeremony->shardOpacity()
                                          : (47.0f + 208 * m_towerCeremony->windowReveal()) / 255;
                }
            }
        }
    }
    updateTowerPortals();
    if (m_cathedralBeam) {
        // During a ceremony CATH_BEAM follows e0863440's drawable bytes: the window's case 7
        // clears its hidden flag with the transparency byte at 255, which case 9 fades to 0.
        // e001ea34 draws it in 2-cycle mode (e001f08c) and, for this 0x400000 drawable with a
        // nonzero byte, blends it with memory in both cycles at the texel's alpha a times the
        // prim alpha p (e001f14c..e001f1e0), so the fade shows (a * p)^2; at 0 it blends once,
        // at a. No draw state here squares the alpha after filtering, so this draws a * p^2: a
        // partly clear texel fades in brighter than the N64's, and reaches a without its step
        // up at count 40.
        const bool scripted = m_towerCeremony && m_towerCeremony->active();
        const f32 alpha = scripted ? m_towerCeremony->cathedralBeamAlpha() : 1;
        m_hiddenWorldObjects[*m_cathedralBeam] =
            scripted ? !m_towerCeremony->cathedralBeamShown() : !completion.cathedralBeam;
        m_worldObjectOpacity[*m_cathedralBeam] = alpha * alpha;
    }
}

void LegendsSceneAdapter::queueTowerCeremonies(const LegendsProgress& before,
                                               const LegendsProgress& after) {
    if (!m_tower || !m_towerCeremony) {
        return;
    }
    m_towerCeremony->begin(before, after);
    if (!m_towerCeremony->pending()) {
        return;
    }
    // initializeParty already installed the owned state. Rewind only its visual
    // representation, never the party's saved awards or completion records: the selected
    // door closes for its ceremony, and the doors it did not select stand open already.
    const auto installed = LegendsTowerProgress::of(m_towerCeremony->displayedProgress());
    for (usize i = 0; i < m_towerDoors.size(); ++i) {
        auto& door = m_towerDoors[i];
        const bool open = installed.doors[i];
        door.motion = LegendsTowerDoorMotion{open};
        m_hiddenFixtures[door.openingFigure] = open && door.openFigure != door.openingFigure;
        m_hiddenFixtures[door.openFigure] = !open;
        m_heldFixtures[door.openingFigure] = !open;
        if (!open) {
            m_fixtures[door.openingFigure].play(0, ItemFigure::Playback::Once);
        }
    }
    updateTowerProgress(false);
}

void LegendsSceneAdapter::startTowerCeremonies(const WorldCamera& camera, const Vec3& attention,
                                               const Vec3& party) {
    if (m_tower && m_towerCeremony) {
        m_towerCeremony->start(camera, attention, party);
        updateTowerCeremony(0);
        placeTowerSumner();
    }
}

const ItemFigure* LegendsSceneAdapter::towerSumner() const {
    return m_tower ? &m_fixtures[m_tower->activeSumner] : nullptr;
}

const ItemFigure* LegendsSceneAdapter::towerStaff() const {
    return m_towerStaff ? &m_fixtures[*m_towerStaff] : nullptr;
}

void LegendsSceneAdapter::updateTowerCeremony(f32 seconds) {
    if (!m_tower || !m_towerCeremony || !m_towerCeremony->presenting()) {
        return;
    }
    auto& tower = *m_tower;
    auto& ceremony = *m_towerCeremony;
    // After the hand-back only Sumner's turn and the staff go on, which placeTowerSumner shows;
    // the idle (updateTower) chooses his gestures again.
    const bool scripted = ceremony.active();
    ceremony.update(seconds);
    if (!scripted) {
        return;
    }
    for (const auto sound : ceremony.takeSounds()) {
        m_soundEvents.push_back({sound, ceremony.sumnerPosition()});
    }
    const auto variant =
        std::ranges::find(tower.sumner, ceremony.gesture(), &TowerPresentation::Variant::sequence);
    if (variant != tower.sumner.end()) {
        if (m_towerGesture != ceremony.gesture()) {
            m_hiddenFixtures[tower.activeSumner] = true;
            m_heldFixtures[tower.activeSumner] = false;
            tower.activeSumner = variant->figure;
            m_hiddenFixtures[tower.activeSumner] = false;
            m_fixtures[tower.activeSumner].play(0, ItemFigure::Playback::Once);
            m_towerGesture = ceremony.gesture();
        }
        m_heldFixtures[tower.activeSumner] = ceremony.active();
        m_fixtures[tower.activeSumner].poseFrame(ceremony.gestureFrame());
    }
    updateTowerProgress(false);
    const auto* entry = ceremony.current();
    for (usize i = 0; i < m_towerRunestones.size(); ++i) {
        if (const auto figure = m_towerRunestones[i]) {
            const bool flying = entry != nullptr &&
                                entry->kind == LegendsTowerCeremony::Kind::Rune &&
                                entry->index == i && ceremony.runeInFlight();
            m_fixtures[*figure].placeAt(flying ? ceremony.runePlacement()
                                               : m_towerRunePlacements[i]);
            if (flying) {
                m_hiddenFixtures[*figure] = false;
            }
            m_fixtureOpacity[*figure] = flying ? ceremony.runeOpacity() : 1;
        }
    }
    if (entry && entry->kind == LegendsTowerCeremony::Kind::Shard) {
        if (const auto figure = m_towerShards[entry->index]) {
            m_fixtureOpacity[*figure] = ceremony.shardOpacity();
        }
    }
    if (!ceremony.active()) {
        m_towerGesture.clear();
    }
}

void LegendsSceneAdapter::placeTowerSumner() {
    if (!m_tower || !m_towerCeremony) {
        return;
    }
    const auto& ceremony = *m_towerCeremony;
    // e002fcf8 turns the one native body, so every gesture's figure stands at his yaw: the one
    // a script or the idle shows next turns with him, untouched by an older placement.
    const Mat4 placement =
        itemPlacement(ceremony.sumnerPosition(), Vec3{0, ceremony.sumnerYaw(), 0});
    for (const auto& variant : m_tower->sumner) {
        m_fixtures[variant.figure].placeAt(placement);
    }
    if (!m_towerStaff) {
        return;
    }
    // The staff hangs from the shown figure's hand node (e0860198) at e0860d1c's local scale.
    m_hiddenFixtures[*m_towerStaff] = !ceremony.staffVisible();
    if (ceremony.staffVisible()) {
        if (const auto hand = m_fixtures[m_tower->activeSumner].nodeTransform(m_towerStaffParent)) {
            m_fixtures[*m_towerStaff].placeAt(*hand * glm::scale(Mat4{1}, ceremony.staffScale()));
        }
    }
}

} // namespace gdl::game
