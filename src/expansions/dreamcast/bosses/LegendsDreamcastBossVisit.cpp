#include "expansions/dreamcast/bosses/LegendsDreamcastBossVisit.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"

#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}
} // namespace

void LegendsDreamcastBossVisit::load(RenderDevice& device, ItemArchive* wizard) {
    if (!wizard) {
        return;
    }
    const auto found = wizard->trees.find("WIZARD");
    if (!found) {
        return;
    }
    const auto& tree = wizard->trees.tree(*found);
    if (tree.sequences.empty() || !m_model.bind(tree, wizard->models, wizard->textures, device)) {
        return;
    }
    m_tree = &tree;
    // Native DoGoodWizard creates WIZARD with drawable flags 0x881880.
    m_model.setAppearance({.unlit = true, .depthWrite = false, .additive = true});
    m_textures.bind(wizard->trees.textureAnimations(), wizard->textures, device);
    if (wizard->trees.find("BOSSKEY") && wizard->trees.find("BOSSKEY2")) {
        // BossEnd (8c0713bc) starts BOSSKEY, then morphs into BOSSKEY2 for
        // thirty seconds. Warm both resources before the frame's first draw.
        EffectTrees::Setting warm;
        warm.depthWrite = false;
        for (const auto* const name : {"BOSSKEY", "BOSSKEY2"}) {
            m_rewards.startSet(device, *wizard, name, Vec3{0}, warm);
        }
        m_rewards.reset();
        m_rewardArchive = wizard;
        m_device = &device;
    }
    if (const auto runes = wizard->trees.find("RUNESTONES")) {
        const auto& runeTree = wizard->trees.tree(*runes);
        if (runeTree.sequences.size() >= 3 &&
            m_runeModel.bind(runeTree, wizard->models, wizard->textures, device)) {
            m_runeTree = &runeTree;
            // Unlike the apparition, the native rune display uses flags 0x1800:
            // its stone meshes retain their authored lighting and depth writes.
        }
    }
}

bool LegendsDreamcastBossVisit::loadSkins(RenderDevice& device,
                                          const std::filesystem::path& nativeRoot) {
    const AssetLocator files{nativeRoot};
    const auto objects = files.find("weapons/objects.rom");
    const auto pixels = files.find("weapons/textures.rom");
    if (!objects || !pixels) {
        return false;
    }
    try {
        const auto decoded =
            formats::decodeLegendsDreamcastTextures(readFile(*objects), readFile(*pixels));
        const auto first =
            std::ranges::find(decoded, "DEATHLIGHT", &formats::LegendsMapTexture::name);
        if (first == decoded.end() || decoded.end() - first < 10) {
            return false;
        }
        std::array<std::unique_ptr<Texture>, 10> skins;
        for (usize frame = 0; frame < skins.size(); ++frame) {
            const auto& texture = first[static_cast<std::ptrdiff_t>(frame)];
            if (texture.image.pixels.empty()) {
                return false;
            }
            skins[frame] = device.createTexture(
                {texture.width, texture.height, TextureFilter::Linear, TextureWrap::Repeat},
                texture.image.pixels);
        }
        m_skins = std::move(skins);
        return true;
    } catch (const std::exception& error) {
        log::warn("Dreamcast victory skin: {}", error.what());
        return false;
    }
}

const Texture* LegendsDreamcastBossVisit::skin() const {
    if (m_skinFrame < 0) {
        return nullptr;
    }
    return m_skins[static_cast<usize>(std::min(m_skinFrame, 9.0f))].get();
}

bool LegendsDreamcastBossVisit::begin(s32 boss, s32 realm, u64 runes, u64 shards,
                                      const Vec3& anchor, std::span<const Vec3> players,
                                      std::optional<Vec3> rewardPosition) {
    if (m_state.stage() != LegendsDreamcastBossCeremony::Stage::None || players.empty() ||
        !finite(anchor) || (rewardPosition && !finite(*rewardPosition)) ||
        !std::ranges::all_of(players, finite) || !m_state.begin(boss, realm, runes, shards)) {
        return false;
    }
    m_position = LegendsDreamcastBossCeremony::wizardPosition(boss, anchor, players);
    m_anchor = anchor;
    m_players.assign(players.begin(), players.end());
    // FaceGoodWizard starts at realm modulo four, then advances to an active player.
    m_facingPlayer = static_cast<usize>(std::max(realm, 0)) % m_players.size();
    facePlayer(0, true);
    if (m_tree) {
        m_player.start(m_tree->sequences.front(), 0);
    }
    if (boss <= 31 && rewardPosition && m_rewardArchive && m_device) {
        const auto& tree = m_rewardArchive->trees.tree(*m_rewardArchive->trees.find("BOSSKEY"));
        const f32 seconds =
            tree.sequences.empty()
                ? 1.0f
                : static_cast<f32>(std::max(tree.sequences.front().frames, 1)) *
                      static_cast<f32>(std::max(tree.sequences.front().frameRate, 1)) / 900;
        m_rewards.startSet(*m_device, *m_rewardArchive, "BOSSKEY", *rewardPosition,
                           {.seconds = seconds + 30,
                            .then = "BOSSKEY2",
                            .morphIn = seconds,
                            .depthWrite = false,
                            .light = std::nullopt});
    }
    if (m_runeTree && boss == 32) {
        m_runePlayer.start(m_runeTree->sequences.front(), 0);
        // HideRunestones (8c06f6c0) addresses meshes, not node descendants.
        constexpr std::string_view kRealms = "CMDF";
        for (usize node = 0; node < m_runeTree->nodes.size(); ++node) {
            const auto& object = m_runeTree->nodes[node].object;
            bool hidden = object == "RUNESTONESCENTE" && !m_state.allRunes();
            for (usize bit = 0; bit < 12; ++bit) {
                if ((runes & (u64{1} << bit)) == 0 &&
                    object == std::format("RUNESTONES{}{}", kRealms[bit / 3], bit % 3 + 1)) {
                    hidden = true;
                }
            }
            if (hidden) {
                m_hiddenRunes.push_back(node);
            }
        }
    }
    capturePresentation();
    return true;
}

void LegendsDreamcastBossVisit::updatePlayers(std::span<const Vec3> players) {
    if (!std::ranges::all_of(players, finite)) {
        return;
    }
    m_players.assign(players.begin(), players.end());
    m_facingPlayer = m_players.empty() ? 0 : m_facingPlayer % m_players.size();
}

void LegendsDreamcastBossVisit::facePlayer(f32 seconds, bool snap) {
    if (m_players.empty()) {
        return;
    }
    const Vec3 direction = m_players[m_facingPlayer] - m_position;
    if (direction.x == 0 && direction.z == 0) {
        return;
    }
    const f32 wanted = portable::atan2(direction.x, direction.z);
    // FaceGoodWizard (8c06f580) clamps the unwrapped difference to pi/2 per second.
    const f32 maximum = glm::half_pi<f32>() * seconds;
    m_yaw += snap ? wanted - m_yaw : std::clamp(wanted - m_yaw, -maximum, maximum);
}

void LegendsDreamcastBossVisit::capturePresentation() {
    m_previousFrame = m_player.presentationFrame();
    m_previousGeneration = m_player.generation();
    m_previousRuneFrame = m_runePlayer.presentationFrame();
    m_previousRuneGeneration = m_runePlayer.generation();
    m_previousOpacity = m_state.wizardAlpha();
    m_previousYaw = m_yaw;
    m_textures.advance(0);
    m_rewards.capturePresentation();
}

std::vector<std::string> LegendsDreamcastBossVisit::pages(usize group) const {
    std::vector<std::string> result;
    if (!m_strings || group >= m_state.groups().size()) {
        return result;
    }
    for (const auto& key : m_state.groups()[group].keys) {
        // Never render localization identifiers as dialogue if optional text is missing.
        result.emplace_back(m_strings->has(key) ? m_strings->get(key) : std::string_view{});
    }
    return result;
}

LegendsDreamcastBossCeremony::Events LegendsDreamcastBossVisit::update(f32 seconds) {
    LegendsDreamcastBossCeremony::Events result;
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 1) {
        return result;
    }
    if (m_skinFrame >= 0) {
        m_skinFrame = std::min(9.0f, m_skinFrame + seconds * 15);
    }
    if (!m_state.running()) {
        return result;
    }
    m_rewards.update(seconds);
    m_tickFraction += static_cast<f64>(seconds) * 60;
    while (m_tickFraction >= 1) {
        m_tickFraction -= 1;
        const bool speaking =
            m_output != nullptr && (!m_voices.empty() || m_output->isPlaying(m_voice));
        const auto event = m_state.step(1, pages(m_state.groupIndex()), speaking);
        if (event.voice) {
            m_voices.push_back(*event.voice);
        }
        if (event.turnToNextPlayer && !m_players.empty()) {
            m_facingPlayer = (m_facingPlayer + 1) % m_players.size();
        }
        if (event.appeared) {
            m_position =
                LegendsDreamcastBossCeremony::wizardPosition(m_state.boss(), m_anchor, m_players);
            facePlayer(0, true);
        }
        if (event.depart && m_runePlayer.playing() && m_state.allRunes()) {
            m_runePlayer.start(m_runeTree->sequences[1], 1);
        }
        // SetSkinFX at 8c070878: DEATHLIGHT, ten frames, rate .5 per
        // 30 Hz player update. This is a body texture, not a particle tree.
        if (event.sparkle) {
            m_skinFrame = 0;
        }
        result.appeared |= event.appeared;
        result.depart |= event.depart;
        result.sparkle |= event.sparkle;
        result.finished |= event.finished;
    }
    if (m_state.wizardAlpha() > 0) {
        facePlayer(seconds);
    }
    if (m_tree && m_state.wizardAlpha() > 0) {
        m_player.advance(seconds, true);
        m_textures.advance(seconds);
    }
    if (m_runePlayer.playing() && m_state.wizardAlpha() > 0) {
        // Native phase 9 force-starts ACTIVE; phase 10 requests ON without
        // interrupting that one-shot sequence (AnimateTree mode 0).
        m_runePlayer.advance(seconds, m_runePlayer.sequence() != 1);
        if (m_runePlayer.sequence() == 1 && m_runePlayer.finished()) {
            m_runePlayer.start(m_runeTree->sequences[2], 2);
        }
    }
    return result;
}

void LegendsDreamcastBossVisit::playSounds(SoundPlayer& output, LegendsDreamcastSounds& bank) {
    if (m_output && m_output != &output) {
        stopSounds();
    }
    m_output = &output;
    while (!m_voices.empty() && !output.isPlaying(m_voice)) {
        m_voice = bank.play(output, m_voices.front());
        m_voices.pop_front();
    }
}

void LegendsDreamcastBossVisit::stopSounds() {
    if (m_output) {
        m_output->stop(m_voice);
    }
    m_voice = kNoSound;
    m_output = nullptr;
    m_voices.clear();
}

void LegendsDreamcastBossVisit::draw(RenderDevice& device, const Mat4& clip,
                                     const WorldCamera& camera, TreeModel::Pass pass,
                                     f32 alpha) const {
    const CameraFrame axes = CameraFrame::of(camera);
    if (TreeModel::compositesEffects(pass)) {
        m_rewards.draw(device, clip, {}, &axes, alpha);
    }
    if (!m_tree || m_state.wizardAlpha() <= 0) {
        return;
    }
    const f32 blend = alpha < 0 || !std::isfinite(alpha) ? 1 : std::clamp(alpha, 0.0f, 1.0f);
    const f32 frame = m_previousGeneration == m_player.generation()
                          ? std::lerp(m_previousFrame, m_player.presentationFrame(), blend)
                          : m_player.presentationFrame();
    TreePose pose;
    pose.evaluate(*m_tree, 0, frame, TreePose::Sampling::Smooth);
    const Mat4 placement = portable::rotate(glm::translate(Mat4{1}, m_position),
                                            std::lerp(m_previousYaw, m_yaw, blend), Vec3{0, 1, 0});
    m_model.setPresentationFrame(0, frame);
    m_textures.apply(m_model, *m_tree, 0, frame, m_textures.presentationOffset(blend));
    m_model.draw(device, clip, placement, {}, pose.matrices(), &axes,
                 std::lerp(m_previousOpacity, m_state.wizardAlpha(), blend), pass);
    if (m_runeTree && m_runePlayer.playing()) {
        const f32 runeFrame =
            m_previousRuneGeneration == m_runePlayer.generation()
                ? std::lerp(m_previousRuneFrame, m_runePlayer.presentationFrame(), blend)
                : m_runePlayer.presentationFrame();
        const u32 sequence = m_runePlayer.sequence();
        TreePose runes;
        runes.evaluate(*m_runeTree, sequence, runeFrame, TreePose::Sampling::Smooth);
        m_runeModel.setPresentationFrame(sequence, runeFrame);
        m_textures.apply(m_runeModel, *m_runeTree, sequence, runeFrame,
                         m_textures.presentationOffset(blend));
        for (const usize node : m_hiddenRunes) {
            m_runeModel.setMeshAlpha(node, 0);
        }
        m_runeModel.draw(device, clip,
                         glm::translate(Mat4{1}, LegendsDreamcastBossCeremony::kRunestonePosition),
                         {}, runes.matrices(), &axes,
                         std::lerp(m_previousOpacity, m_state.wizardAlpha(), blend), pass);
    }
}

void LegendsDreamcastBossVisit::drawCaption(Canvas& canvas, const TextPainter& font,
                                            f32 width) const {
    const auto caption = m_state.caption();
    if (!caption || !font.ready()) {
        return;
    }
    const auto text = pages(caption->group);
    if (caption->page >= text.size()) {
        return;
    }
    const auto& page = text[caption->page];
    const TextStyle style{.scale = LegendsDreamcastBossCeremony::kCaptionScale};
    s32 y = static_cast<s32>(LegendsDreamcastBossCeremony::kCaptionY);
    usize offset = 0;
    for (const auto& line : ScrollBox::splitLines(page)) {
        const usize count = caption->shown > offset ? caption->shown - offset : 0;
        // CaptionText centres the complete line once; revealing more characters
        // must not make the already-written prefix slide across the screen.
        const s32 left = static_cast<s32>(width * .5f) - font.measure(line, style.scale) / 2;
        font.draw(canvas, left, y, std::string_view{line}.substr(0, count), style);
        offset += line.size() + 1;
        y += static_cast<s32>(LegendsDreamcastBossCeremony::kCaptionLineHeight);
    }
}

} // namespace gdl::game
