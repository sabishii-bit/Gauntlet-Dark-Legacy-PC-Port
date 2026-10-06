#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"

namespace gdl {
struct AssetAuditIssue {
    std::string record;
    std::string detail;
    bool dependency = false; ///< requires the consumer's load context/policy to adjudicate
};

/** Completeness of one archive, not a certificate of visual or gameplay parity. */
struct AssetAuditResult {
    AssetAuditResult() = default;
    ~AssetAuditResult() = default;
    AssetAuditResult(const AssetAuditResult&) = default;
    AssetAuditResult& operator=(const AssetAuditResult&) = default;
    // std::map's move may allocate its sentinel on MSVC.
    AssetAuditResult(AssetAuditResult&&) noexcept(false) = default;
    AssetAuditResult& operator=(AssetAuditResult&&) noexcept(false) = default;

    std::filesystem::path directory;
    bool dependenciesChecked = false;
    usize models = 0;
    usize images = 0;
    usize placeholders = 0;
    usize externalReferences = 0;
    usize trees = 0;
    usize animations = 0;
    usize worldObjects = 0;
    std::map<u32, usize> objectFlags; ///< observed combinations, including unclassified bits
    std::map<s32, usize> animationModes;
    std::vector<AssetAuditIssue> issues;

    bool passed() const;
};

/** Eagerly decodes native assets and checks references with the runtime's texture
 * resolver. Dependency checks require the same ordered lenders as the consumer;
 * a decode-only census explicitly leaves those checks incomplete. No GPU is needed. */
AssetAuditResult auditAssets(const std::filesystem::path& directory,
                             std::span<TextureSet* const> lenders = {},
                             bool checkDependencies = true);
} // namespace gdl
