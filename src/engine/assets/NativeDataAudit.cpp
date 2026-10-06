#include "engine/assets/NativeDataAudit.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <set>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"
#include "engine/world/TreePose.h"

#include "formats/AnimationTree.h"
#include "formats/AudioRom.h"
#include "formats/CritterWad.h"
#include "formats/PlayerDataWad.h"
#include "formats/ShopWad.h"
#include "formats/SoundBank.h"
#include "formats/TextRom.h"
#include "formats/WadDirectory.h"
#include "formats/WorldDataWad.h"

namespace gdl {
namespace {
bool finite(const Mat4& matrix) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

void auditAnimation(NativeDataAuditResult& result) {
    const auto native = formats::AnimationFile::parse(readFile(result.file));
    result.counts["trees"] = native.trees.size();
    result.counts["texture_animations"] = native.textureAnimations.size();
    result.counts["particle_templates"] = native.particles.size();
    AnimationSet animations;
    if (!animations.load(result.file.parent_path())) {
        if (!native.trees.empty() || !native.textureAnimations.empty()) {
            result.issues.push_back({"archive", "runtime animation conversion failed"});
        }
        return;
    }
    for (u32 t = 0; t < animations.size(); ++t) {
        const auto& tree = animations.tree(t);
        TreePose pose;
        pose.rest(tree);
        if (!std::ranges::all_of(pose.matrices(), finite)) {
            result.issues.push_back({tree.name, "non-finite rest pose"});
            continue;
        }
        result.counts["nodes"] += tree.nodes.size();
        for (u32 s = 0; s < tree.sequences.size(); ++s) {
            const auto& sequence = tree.sequences[s];
            const auto record = std::format("tree {} sequence {}", tree.name, sequence.name);
            ++result.counts["sequences"];
            result.counts["tracks"] += sequence.tracks.size();
            std::set<f32> frames{0.0f, static_cast<f32>(std::max(0, sequence.frames - 1))};
            bool valid = true;
            for (const auto& track : sequence.tracks) {
                result.counts["keys"] += track.frames.size();
                if (!std::ranges::all_of(track.values,
                                         [](f32 value) { return std::isfinite(value); })) {
                    result.issues.push_back({record, "non-finite track values"});
                    valid = false;
                    break;
                }
                for (usize k = 0; k < track.frames.size(); ++k) {
                    frames.insert(static_cast<f32>(track.frames[k]));
                    if (k != 0) {
                        frames.insert((static_cast<f32>(track.frames[k - 1]) +
                                       static_cast<f32>(track.frames[k])) *
                                      0.5f);
                    }
                }
            }
            if (!valid) {
                continue;
            }
            for (const f32 frame : frames) {
                pose.evaluate(tree, s, frame, false, true);
                ++result.counts["pose_samples"];
                if (!std::ranges::all_of(pose.matrices(), finite)) {
                    result.issues.push_back({record, std::format("non-finite pose at {}", frame)});
                    break;
                }
            }
        }
    }
}

void auditSound(NativeDataAuditResult& result) {
    const auto bank = formats::SoundBank::parse(readFile(result.file));
    result.counts["calls"] = bank.calls.size();
    result.counts["samples"] = bank.samples.size();
    SoundSet sounds;
    // Exercise the exact runtime sample conversion, without making this
    // structural audit dependent on a subjective restoration policy.
    if (!sounds.load(result.file, SoundSet::Restoration::Disabled)) {
        result.issues.push_back({"archive", "runtime sound conversion failed"});
        return;
    }
    for (u32 s = 0; s < bank.samples.size(); ++s) {
        const auto record = std::format("sample[{}] {}", s, bank.samples[s].name);
        try {
            const auto& clip = sounds.sample(s);
            if (clip.sampleRate == 0 || clip.samples.empty() ||
                (bank.samples[s].sampleCount != 0 &&
                 clip.samples.size() != bank.samples[s].sampleCount)) {
                result.issues.push_back({record, "invalid rate, empty or truncated decoded PCM"});
            }
            result.counts["pcm_samples"] += clip.samples.size();
        } catch (const std::exception& e) {
            result.issues.push_back({record, e.what()});
        }
    }
    for (u32 c = 0; c < sounds.size(); ++c) {
        const auto& call = sounds.entry(c);
        result.counts["steps"] += call.sequence.size();
        if (call.sequence.empty()) {
            result.issues.push_back(
                {std::format("call[{}] {}", c, call.name), "named sound has no playable sequence"});
        }
    }
}

void auditWad(NativeDataAuditResult& result) {
    const auto bytes = readFile(result.file);
    for (const auto& section : formats::readWadDirectory(bytes, result.file.string())) {
        result.counts["section_" + section.tag] += section.count;
    }
    if (result.kind == "player_wad") {
        const auto player = formats::parsePlayerDataWad(bytes);
        result.counts["effects"] = player.effects.size();
        result.counts["strikes"] = player.strikes.size();
        if (player.effects.size() != player.effectCount ||
            player.strikes.size() != player.damageCount) {
            result.issues.push_back({"PDAT", "declared effects/strikes are missing"});
        }
        for (usize move = 0; move < player.moves.size(); ++move) {
            if (player.moves[move] >= 0 &&
                static_cast<usize>(player.moves[move]) >= player.strikes.size()) {
                result.issues.push_back({std::string(formats::PlayerClassRecord::kMoveNames[move]),
                                         "strike index outside DAMG"});
            }
        }
    } else if (result.kind == "world_wad") {
        const auto world = formats::WorldDataFile::parse(bytes);
        result.counts["levels"] = world.levels.size();
        result.counts["cameras"] = world.cameras.size();
        result.counts["boss_cameras"] = world.bossCameras.size();
        result.counts["audio"] = world.audio.size();
        result.counts["enemies"] = world.enemies.size();
    } else if (result.kind == "critter_wad") {
        const auto critter = formats::parseCritterWad(bytes);
        result.counts["types"] = critter.types.size();
        result.counts["moves"] = critter.moves.size();
        result.counts["patterns"] = critter.patterns.size();
        result.counts["damages"] = critter.damages.size();
        result.counts["sounds"] = critter.sounds.size();
    } else {
        result.counts["items"] = formats::parseShopWad(bytes).size();
    }
}
} // namespace

std::string nativeDataKind(const std::filesystem::path& file) {
    const auto name = toLowerAscii(file.filename().string());
    const auto extension = toLowerAscii(file.extension().string());
    const auto parent = toLowerAscii(file.parent_path().filename().string());
    if (name == "anim.ps2") {
        return "animation";
    }
    if (extension == ".vbk") {
        return "sound_bank";
    }
    if (name == "audatps2.rom") {
        return "audio_directory";
    }
    if (extension == ".rom" && parent == "text") {
        return "text";
    }
    if (extension == ".wad") {
        if (parent == "pdata") {
            return "player_wad";
        }
        if (parent == "wdata") {
            return "world_wad";
        }
        if (parent == "critter") {
            return "critter_wad";
        }
        if (parent == "shpdata") {
            return "shop_wad";
        }
    }
    return {};
}

NativeDataAuditResult auditNativeData(const std::filesystem::path& file) {
    NativeDataAuditResult result;
    result.file = file;
    result.kind = nativeDataKind(file);
    try {
        if (result.kind == "animation") {
            auditAnimation(result);
        } else if (result.kind == "sound_bank") {
            auditSound(result);
        } else if (result.kind == "audio_directory") {
            const auto directory = formats::AudioRom::parse(readFile(file));
            result.counts["banks"] = directory.banks.size();
            result.counts["sounds"] = directory.sounds.size();
        } else if (result.kind == "text") {
            const auto text = formats::TextRom::parse(readFile(file));
            result.counts["fonts"] = text.fonts.size();
            result.counts["messages"] = text.messages.size();
            result.counts["lists"] = text.lists.size();
        } else if (!result.kind.empty()) {
            auditWad(result);
        } else {
            result.issues.push_back({"file", "unsupported native data family"});
        }
    } catch (const std::exception& e) {
        result.issues.push_back({"file", e.what()});
    }
    return result;
}
} // namespace gdl
