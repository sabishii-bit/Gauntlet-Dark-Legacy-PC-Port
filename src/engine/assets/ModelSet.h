#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/render/Mesh.h"

namespace gdl {

struct ModelSetEntry {
    std::string name;
    std::filesystem::path file; ///< empty when the object has no exported mesh
    std::uint32_t triangles = 0;
};

/** One unpacked archive's objects: named meshes loaded from their OBJ files on first use. */
class ModelSet {
public:
    /** Reads `directory/objects.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }
    const ModelSetEntry& entry(std::uint32_t index) const;
    std::optional<std::uint32_t> find(std::string_view name) const;

    /** The object's mesh; throws FileError or FormatError when its file cannot be read. */
    const Mesh& mesh(std::uint32_t index);

private:
    std::vector<ModelSetEntry> m_entries;
    std::unordered_map<std::string, std::uint32_t> m_byName;
    std::vector<Mesh> m_meshes;
    std::vector<bool> m_loaded;
};

} // namespace gdl
