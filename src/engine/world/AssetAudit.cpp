#include "engine/world/AssetAudit.h"

#include <algorithm>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <utility>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureBindings.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/world/ParticleSystem.h"

#include "formats/AnimationTree.h"
#include "formats/ModelArchive.h"

namespace gdl {
bool AssetAuditResult::passed() const {
    return std::ranges::none_of(issues, [&](const AssetAuditIssue& issue) {
        return dependenciesChecked || !issue.dependency;
    });
}

AssetAuditResult auditAssets(const std::filesystem::path& directory,
                             std::span<TextureSet* const> lenders, bool checkDependencies) {
    AssetAuditResult result;
    result.directory = directory;
    result.dependenciesChecked = checkDependencies;
    const auto issue = [&](std::string record, std::string detail, bool dependency = false) {
        result.issues.push_back({std::move(record), std::move(detail), dependency});
    };
    const auto attempt = [&](const std::string& record, const auto& action) {
        try {
            action();
        } catch (const std::exception& e) {
            issue(record, e.what());
        }
    };
    ModelSet models;
    TextureSet textures;
    const AssetLocator locator(directory);
    if (!locator.find("objects.ngc")) {
        issue("objects.ngc", "native model archive is missing");
        return result;
    }
    const bool modelsLoaded = models.load(directory);
    const bool texturesLoaded = textures.load(directory);
    if (!modelsLoaded || !texturesLoaded) {
        // Runtime loaded() asks whether there is usable content, not whether an
        // empty table parsed correctly. Texture-only archives are legitimate.
        attempt("objects.ngc/textures.ngc", [&] {
            const auto parsed =
                formats::ModelArchive::parse(readFile(*locator.find("objects.ngc")));
            if ((!modelsLoaded && (!parsed.objects().empty() || !parsed.objectDefs().empty())) ||
                (!texturesLoaded && (!parsed.bitmaps().empty() || !parsed.bitmapDefs().empty()))) {
                issue("objects.ngc/textures.ngc",
                      "native archive could not be loaded; see loader diagnostic");
            }
        });
        if (!result.passed()) {
            return result;
        }
    }
    const TextureBindings bindings(textures, lenders);
    const auto image = [&](const std::optional<TextureBinding>& binding,
                           const std::string& record) {
        if (!binding) {
            issue(record, "unresolved texture in the supplied archive context", true);
            return;
        }
        attempt(record, [&] { binding->set->image(binding->index); });
    };
    for (u32 i = 0; i < textures.size(); ++i) {
        const auto& entry = textures.entry(i);
        const auto record = std::format("bitmap[{}] {}", i, entry.name);
        if (entry.noPicture) {
            ++result.placeholders;
        } else if (entry.external()) {
            ++result.externalReferences;
            if (checkDependencies) {
                image(bindings.slot(i), record);
            }
        } else {
            attempt(record, [&] { textures.image(i); });
            ++result.images;
        }
    }
    for (u32 i = 0; i < models.size(); ++i) {
        const auto record = std::format("model[{}] {}", i, models.entry(i).name);
        attempt(record, [&] {
            const auto& mesh = models.mesh(i);
            for (const auto& part : mesh.parts) {
                if (part.texture >= textures.size() ||
                    (part.lightmap != 0 && part.lightmap >= textures.size())) {
                    issue(record, "mesh texture/lightmap index is outside its owning archive");
                }
            }
        });
        ++result.models;
    }
    const auto particle = [&](const ParticleTemplate& source, const std::string& record) {
        if (checkDependencies) {
            const auto descriptor = ParticleDescriptor::fromTemplate(source);
            image(bindings.named(descriptor.texture), record + " texture " + descriptor.texture);
        }
    };
    if (const auto animationFile = locator.find("ANIM.PS2")) {
        AnimationSet animations;
        const bool loaded = animations.load(directory);
        if (!loaded) {
            attempt("ANIM.PS2", [&] {
                const auto parsed = formats::AnimationFile::parse(readFile(*animationFile));
                if (!parsed.trees.empty() || !parsed.textureAnimations.empty()) {
                    issue("ANIM.PS2", "native animation archive could not be loaded");
                }
            });
        }
        if (loaded) {
            for (const auto& source : animations.textureAnimations()) {
                ++result.animations;
                ++result.animationModes[source.source >= 0 ? 0 : source.source];
                const auto record =
                    std::format("texmod[{}] {}", result.animations - 1, source.name);
                // Fades target a tree's alpha, and -6 explicitly has no update.
                if (source.fades() || source.source == -6 || source.frames == 0) {
                    continue;
                }
                if (source.texture < 0 || static_cast<usize>(source.texture) >= textures.size()) {
                    issue(record, "destination texture slot is outside its owning archive");
                    continue;
                }
                if ((!source.cycles() && !source.scrolls()) ||
                    source.frames == std::numeric_limits<s32>::min()) {
                    issue(record, "unsupported animation mode or frame count");
                    continue;
                }
                if (source.scrolls() || (!checkDependencies && source.source < 0)) {
                    continue;
                }
                const auto first =
                    source.source >= 0
                        ? std::optional{TextureBinding{&textures, static_cast<u32>(source.source)}}
                        : bindings.image(source.frameName);
                if (!first) {
                    issue(record, "unresolved source frames: " + source.frameName, true);
                    continue;
                }
                const auto count =
                    static_cast<usize>(source.frames < 0 ? -source.frames : source.frames);
                if (first->index >= first->set->size() ||
                    count > first->set->size() - first->index) {
                    issue(record,
                          std::format("{} requests {} frames at {} in {} ({} slots)",
                                      source.frameName, count, first->index,
                                      first->set->directory().string(), first->set->size()));
                    continue;
                }
                for (usize frame = 0; frame < count; ++frame) {
                    image(TextureBinding{first->set, first->index + static_cast<u32>(frame)},
                          std::format("{} frame {}", record, frame));
                }
            }
            for (u32 i = 0; i < animations.size(); ++i) {
                const auto& tree = animations.tree(i);
                ++result.trees;
                for (const auto& node : tree.nodes) {
                    ++result.objectFlags[node.objectFlags];
                    const auto record = std::format("tree {} node {}", tree.name, node.name);
                    if (node.name != "DUMMY" && node.name != "NULL1") {
                        if (!node.object.empty() && !models.find(node.object)) {
                            issue(record,
                                  "unresolved model (consumer may supply an empty node): " +
                                      node.object,
                                  true);
                        }
                        for (usize sequence = 0; sequence < node.objectFrames.size(); ++sequence) {
                            const auto& run = node.objectFrames[sequence];
                            if (run.object.empty()) {
                                continue;
                            }
                            const auto start = models.find(run.object);
                            if (!start || run.frames < 0 ||
                                static_cast<usize>(run.frames) > models.size() - *start) {
                                issue(record,
                                      std::format("sequence {} has incomplete object frames: {}",
                                                  sequence, run.object),
                                      !start && run.frames >= 0);
                            }
                        }
                    }
                    if (node.particle >= 0) {
                        if (static_cast<usize>(node.particle) >=
                            animations.particleTemplates().size()) {
                            issue(record, "particle template index outside archive");
                        } else {
                            particle(
                                animations.particleTemplates()[static_cast<usize>(node.particle)],
                                record);
                        }
                    }
                }
            }
        }
    }
    if (locator.find("WORLDS.PS2")) {
        WorldLayout layout;
        if (!layout.load(directory)) {
            issue("WORLDS.PS2", "native world layout could not be loaded");
        } else {
            for (const auto& object : layout.objects()) {
                ++result.worldObjects;
                ++result.objectFlags[object.objectFlags];
                if (object.particles()) {
                    const auto tag = object.name.find("PSYS");
                    const auto* source = tag != std::string::npos && tag + 4 < object.name.size()
                                             ? layout.findParticleTemplate(object.name[tag + 4])
                                             : nullptr;
                    if (source == nullptr) {
                        issue("world " + object.name, "missing particle template", true);
                    } else {
                        particle(*source, "world " + object.name);
                    }
                }
            }
        }
    }
    return result;
}
} // namespace gdl
