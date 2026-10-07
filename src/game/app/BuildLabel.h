#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "engine/core/SpecialMembers.h"
#include "engine/render/RenderDevice.h"

namespace gdl::game {

/** Always-on presentation overlay; its version is baked from VERSION at build time. */
class BuildLabel {
public:
    explicit BuildLabel(std::string_view version);
    ~BuildLabel() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(BuildLabel);

    static std::string formatVersion(std::string_view version);
    static Mat4 projection(Extent2D extent);

    bool load(RenderDevice& device);
    void release();
    void render(RenderDevice& device, Extent2D extent) const;
    const std::string& text() const { return m_text; }
    bool ready() const { return m_texture != nullptr; }

private:
    std::string m_text;
    std::unique_ptr<Texture> m_texture;
};

} // namespace gdl::game
