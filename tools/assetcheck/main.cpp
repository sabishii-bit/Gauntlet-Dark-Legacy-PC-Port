#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/world/AssetAudit.h"

#include "DataAudit.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelItemArchives.h"

namespace {
using namespace gdl;

void printLine(std::FILE* stream, const std::string& line) {
    std::fputs(line.c_str(), stream);
    std::fputc('\n', stream);
}

struct ArchiveRequest {
    std::filesystem::path directory;
    std::optional<game::LevelRef> level;
};

int run(std::span<char*> args) {
    if (args.size() < 2 || std::string_view(args[1]) == "--help") {
        std::puts("assetcheck DIRECTORY [--lender DIRECTORY]... [--recursive --decode-only] "
                  "[--report FILE]\n"
                  "assetcheck GAME_ROOT --levels [--report FILE]\n"
                  "assetcheck GAME_ROOT --data [--report FILE]\n"
                  "A single archive checks dependencies in the supplied lender order.\n"
                  "Recursive decode-only mode inventories native archives; it does not certify "
                  "bindings.\n"
                  "--levels checks catalogued level geometry using the runtime's item lenders; "
                  "known absent retail particle templates remain explicit findings.\n"
                  "--data checks all native ANIM, WAD, ROM and VBK files, including standalone "
                  "player action archives and lazy audio samples.\n"
                  "No assets are changed or exported. Exit 1 means findings; 2 means invalid "
                  "invocation.");
        return args.size() < 2 ? 2 : 0;
    }
    const std::filesystem::path root(args[1]);
    std::filesystem::path report;
    std::vector<std::filesystem::path> lenderPaths;
    bool recursive = false;
    bool decodeOnly = false;
    bool levels = false;
    bool data = false;
    for (usize i = 2; i < args.size(); ++i) {
        const std::string_view arg(args[i]);
        if (arg == "--recursive") {
            recursive = true;
        } else if (arg == "--decode-only") {
            decodeOnly = true;
        } else if (arg == "--levels") {
            levels = true;
        } else if (arg == "--data") {
            data = true;
        } else if ((arg == "--lender" || arg == "--report") && i + 1 < args.size()) {
            const std::filesystem::path value(args[++i]);
            if (arg == "--report") {
                report = value;
            } else {
                lenderPaths.push_back(value);
            }
        } else {
            printLine(stderr, std::format("Unknown or incomplete argument: {}", args[i]));
            return 2;
        }
    }
    if (!std::filesystem::is_directory(root) || (recursive && !decodeOnly) ||
        ((levels || data) && (recursive || decodeOnly || !lenderPaths.empty())) ||
        (levels && data)) {
        std::fputs("Supply an existing directory; recursive mode requires --decode-only; "
                   "--levels and --data cannot combine other modes.\n",
                   stderr);
        return 2;
    }
    if (data) {
        return runDataAudit(root, report);
    }
    std::vector<TextureSet> sets(lenderPaths.size());
    std::vector<TextureSet*> lenders;
    for (usize i = 0; i < sets.size(); ++i) {
        if (!sets[i].load(lenderPaths[i])) {
            return 2;
        }
        lenders.push_back(&sets[i]);
    }
    std::vector<ArchiveRequest> archives;
    nlohmann::json excluded = nlohmann::json::array();
    if (levels) {
        game::LevelCatalog catalog;
        if (!catalog.load(root)) {
            std::fputs("No level catalogue found.\n", stderr);
            return 2;
        }
        for (const auto& realm : catalog.realms()) {
            for (const auto& name : realm.levels) {
                const auto ref = catalog.byName(name);
                if (!ref) {
                    return 2;
                }
                archives.push_back({root / ref->directory, ref});
            }
        }
    } else if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() &&
                toLowerAscii(entry.path().filename().string()) == "objects.ngc") {
                const auto relative =
                    toLowerAscii(entry.path().lexically_relative(root).generic_string());
                if (relative == "items/demo/objects.ngc") {
                    const auto bytes = readFile(entry.path());
                    constexpr usize kVersionOffset = 64;
                    constexpr u32 kLegacyVersion = 0xF00B0004;
                    if (bytes.size() >= kVersionOffset + sizeof(u32) &&
                        readU32LE(bytes, kVersionOffset) == kLegacyVersion) {
                        excluded.push_back(
                            {{"path", relative},
                             {"reason", "legacy v4 demo archive; not used by runtime"}});
                        continue;
                    }
                }
                archives.push_back({entry.path().parent_path(), std::nullopt});
            }
        }
    } else {
        archives.push_back({root, std::nullopt});
    }
    std::ranges::sort(archives, {}, &ArchiveRequest::directory);
    if (archives.empty()) {
        std::fputs("No native archives found; an empty scan is not a pass.\n", stderr);
        return 2;
    }
    nlohmann::json context = nlohmann::json::array();
    for (const auto& lender : lenderPaths) {
        context.push_back(lender.generic_string());
    }
    nlohmann::json output{
        {"schema", 1},
        {"scope", levels
                      ? "catalogued level geometry/effects with runtime item texture lenders; "
                        "not actor or item animation binding contexts"
                      : "objects.ngc archives, companion textures and local ANIM.PS2/WORLDS.PS2"},
        {"dependencies_checked", !decodeOnly},
        {"level_contexts", levels},
        {"lenders_in_order", context},
        {"excluded", excluded},
        {"archives", nlohmann::json::array()}};
    usize findings = 0;
    usize errors = 0;
    for (const auto& request : archives) {
        const auto& directory = request.directory;
        game::LevelItemArchives items;
        auto activeLenders = lenders;
        bool itemsLoaded = true;
        if (request.level) {
            itemsLoaded = items.load(root, *request.level);
            activeLenders = items.textureLenders();
        }
        auto result = auditAssets(directory, activeLenders, !decodeOnly);
        if (!itemsLoaded) {
            result.issues.push_back({"item archives", "runtime item context failed to load", true});
        }
        nlohmann::json activeContext = nlohmann::json::array();
        for (const auto* lender : activeLenders) {
            activeContext.push_back(lender->directory().generic_string());
        }
        nlohmann::json issues = nlohmann::json::array();
        for (const auto& issue : result.issues) {
            const bool error = !issue.dependency || !decodeOnly;
            errors += error ? 1 : 0;
            issues.push_back({{"record", issue.record},
                              {"detail", issue.detail},
                              {"category", issue.dependency ? "dependency" : "decode"},
                              {"severity", error ? "error" : "review"}});
            if (report.empty()) {
                printLine(stdout,
                          std::format("{}: {}: {}: {}", directory.string(),
                                      error ? "error" : "review", issue.record, issue.detail));
            }
        }
        output["archives"].push_back({{"directory", directory.generic_string()},
                                      {"level", request.level ? request.level->name : ""},
                                      {"lenders_in_order", activeContext},
                                      {"models", result.models},
                                      {"images", result.images},
                                      {"placeholders", result.placeholders},
                                      {"external_references", result.externalReferences},
                                      {"trees", result.trees},
                                      {"animations", result.animations},
                                      {"world_objects", result.worldObjects},
                                      {"object_flags", result.objectFlags},
                                      {"animation_modes", result.animationModes},
                                      {"issues", issues}});
        findings += result.issues.size();
        if (!result.issues.empty()) {
            printLine(stderr,
                      std::format("{}: {} findings", directory.string(), result.issues.size()));
        }
    }
    output["findings"] = findings;
    output["errors"] = errors;
    if (!report.empty()) {
        writeTextFile(report, output.dump(2));
    }
    printLine(stdout, std::format("{} archives, {} errors, {} findings. Dependencies: {}.",
                                  archives.size(), errors, findings,
                                  decodeOnly ? "NOT CHECKED (decode inventory only)"
                                             : "checked in supplied context"));
    return errors == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(std::span(argv, static_cast<usize>(argc)));
    } catch (const std::exception& e) {
        std::fputs("assetcheck: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        return 2;
    } catch (...) {
        std::fputs("assetcheck: unexpected failure\n", stderr);
        return 2;
    }
}
