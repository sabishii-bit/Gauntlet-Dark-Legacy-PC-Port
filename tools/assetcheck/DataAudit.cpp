#include "DataAudit.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/assets/NativeDataAudit.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

int runDataAudit(const std::filesystem::path& root, const std::filesystem::path& report) {
    using namespace gdl;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = toLowerAscii(entry.path().extension().string());
        if (toLowerAscii(entry.path().filename().string()) == "anim.ps2" || extension == ".wad" ||
            extension == ".rom" || extension == ".vbk") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    if (files.empty()) {
        std::fputs("No native data files found; an empty scan is not a pass.\n", stderr);
        return 2;
    }
    nlohmann::json output{{"schema", 1},
                          {"scope",
                           "ANIM poses at keys and fractional midpoints; VBK PCM and call tables; "
                           "typed WAD and text/audio ROM readers. No cross-archive visual/audio "
                           "binding, playback mixing, or gameplay parity certification."},
                          {"dependencies_checked", false},
                          {"archives", nlohmann::json::array()},
                          {"kinds", nlohmann::json::object()}};
    usize errors = 0;
    for (const auto& file : files) {
        const auto result = auditNativeData(file);
        nlohmann::json issues = nlohmann::json::array();
        for (const auto& issue : result.issues) {
            issues.push_back({{"record", issue.record}, {"detail", issue.detail}});
            if (report.empty()) {
                const auto line =
                    std::format("{}: {}: {}\n", file.string(), issue.record, issue.detail);
                std::fputs(line.c_str(), stdout);
            }
        }
        const auto kind = result.kind.empty() ? "unsupported" : result.kind;
        output["kinds"][kind] = output["kinds"].value(kind, usize{0}) + 1;
        output["archives"].push_back({{"file", file.generic_string()},
                                      {"kind", kind},
                                      {"counts", result.counts},
                                      {"issues", issues}});
        errors += result.issues.size();
    }
    output["errors"] = errors;
    output["findings"] = errors;
    if (!report.empty()) {
        writeTextFile(report, output.dump(2));
    }
    const auto line = std::format("{} native data files, {} errors. Cross-archive dependencies "
                                  "NOT CHECKED.\n",
                                  files.size(), errors);
    std::fputs(line.c_str(), stdout);
    return errors == 0 ? 0 : 1;
}
