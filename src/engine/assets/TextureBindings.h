#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"

namespace gdl {

/** An image's owning archive and index; borrows both for the binding's lifetime. */
struct TextureBinding {
    TextureSet* set = nullptr;
    u32 index = 0;
};

/** Resolves only the archives explicitly in a consumer's load context, in order.
 * External references never resolve to other external references. */
class TextureBindings {
public:
    TextureBindings(TextureSet& primary, std::span<TextureSet* const> lenders = {})
        : m_primary(primary), m_lenders(lenders) {}

    /** A material slot keeps its intentional empty animation placeholder. */
    std::optional<TextureBinding> slot(u32 index) const;
    /** A named material/particle slot; local animation placeholders remain valid. */
    std::optional<TextureBinding> named(std::string_view name) const;
    /** An animation source needs actual pixels, not an empty destination slot. */
    std::optional<TextureBinding> image(std::string_view name) const;

private:
    TextureSet& m_primary;
    std::span<TextureSet* const> m_lenders;
};
} // namespace gdl
