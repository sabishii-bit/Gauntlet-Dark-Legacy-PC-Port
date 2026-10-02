#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Mesh.h"

#include "formats/ModelArchive.h"

namespace gdl {

struct ModelSetEntry {
    std::string name;
    u32 triangles = 0;
};

/** Named meshes decoded on first use from a native archive. */
class ModelSet {
public:
    /** Loads objects.ngc from the supplied directory. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_entries.empty(); }
    usize size() const { return m_entries.size(); }
    const ModelSetEntry& entry(u32 index) const;
    std::optional<u32> find(std::string_view name) const;

    /** The object's mesh; throws FormatError when its geometry cannot be decoded. */
    const Mesh& mesh(u32 index);

private:
    bool loadNative(const std::filesystem::path& file);
    std::optional<formats::ModelArchive> m_native;
    std::vector<ModelSetEntry> m_entries;
    std::unordered_map<std::string, u32> m_byName;
    std::vector<Mesh> m_meshes;
    std::vector<bool> m_loaded;
};

} // namespace gdl
