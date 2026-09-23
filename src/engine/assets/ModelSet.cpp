#include "engine/assets/ModelSet.h"

#include <cstdint>
#include <exception>

#include <nlohmann/json.hpp>

#include "engine/assets/ObjModel.h"
#include "engine/core/Assert.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::string_view kManifestName = "objects.json";

} // namespace

bool ModelSet::load(const std::filesystem::path& directory) {
    m_entries.clear();
    m_byName.clear();
    m_meshes.clear();
    m_loaded.clear();
    const std::filesystem::path manifest = directory / kManifestName;
    try {
        const std::vector<std::uint8_t> bytes = readFile(manifest);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const nlohmann::json& object : root.at("objects")) {
            ModelSetEntry entry;
            entry.name = normalizeAssetName(object.value("name", std::string{}));
            if (object.contains("file")) {
                entry.file = directory / object.at("file").get<std::string>();
            }
            entry.triangles = object.value("meshTriangles", 0U);
            m_entries.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        log::warn("Model set {}: {}", manifest.string(), e.what());
        m_entries.clear();
        return false;
    }
    for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
        if (!m_entries[i].name.empty()) {
            m_byName.try_emplace(m_entries[i].name, i);
        }
    }
    m_meshes.resize(m_entries.size());
    m_loaded.assign(m_entries.size(), false);
    return !m_entries.empty();
}

const ModelSetEntry& ModelSet::entry(std::uint32_t index) const {
    GDL_VERIFY(index < m_entries.size(), "model index out of range");
    return m_entries[index];
}

std::optional<std::uint32_t> ModelSet::find(std::string_view name) const {
    const auto it = m_byName.find(normalizeAssetName(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

const Mesh& ModelSet::mesh(std::uint32_t index) {
    GDL_VERIFY(index < m_entries.size(), "model index out of range");
    if (!m_loaded[index]) {
        if (m_entries[index].file.empty()) {
            throw FileError("object " + m_entries[index].name + " has no mesh file");
        }
        m_meshes[index] = loadObj(m_entries[index].file);
        m_loaded[index] = true;
    }
    return m_meshes[index];
}

} // namespace gdl
