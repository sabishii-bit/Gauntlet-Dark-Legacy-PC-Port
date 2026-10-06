#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
struct NativeDataIssue {
    std::string record;
    std::string detail;
};

/** Decoder and internal-reference checks, not a visual/behavioral parity verdict. */
struct NativeDataAuditResult {
    NativeDataAuditResult() = default;
    ~NativeDataAuditResult() = default;
    NativeDataAuditResult(const NativeDataAuditResult&) = default;
    NativeDataAuditResult& operator=(const NativeDataAuditResult&) = default;
    NativeDataAuditResult(NativeDataAuditResult&&) noexcept(false) = default;
    NativeDataAuditResult& operator=(NativeDataAuditResult&&) noexcept(false) = default;

    std::filesystem::path file;
    std::string kind;
    std::map<std::string, usize> counts;
    std::vector<NativeDataIssue> issues;
    bool passed() const { return issues.empty(); }
};

/** Recognized native data families; an unknown WAD/ROM is explicitly unsupported. */
std::string nativeDataKind(const std::filesystem::path& file);
/** Eager audio decoding and integer/fractional animation poses use runtime readers.
 * WAD checks cover the fields the typed readers implement, not unmodeled fields.
 * This does not check cross-archive models, texture lenders or gameplay dispatch. */
NativeDataAuditResult auditNativeData(const std::filesystem::path& file);
} // namespace gdl
