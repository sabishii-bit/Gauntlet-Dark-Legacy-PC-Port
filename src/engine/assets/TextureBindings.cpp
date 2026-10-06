#include "engine/assets/TextureBindings.h"

namespace gdl {
std::optional<TextureBinding> TextureBindings::image(std::string_view name) const {
    const auto find = [&](TextureSet& set) -> std::optional<TextureBinding> {
        const auto index = set.find(name);
        if (index && !set.entry(*index).external() && !set.entry(*index).noPicture) {
            return TextureBinding{&set, *index};
        }
        return std::nullopt;
    };
    if (auto found = find(m_primary)) {
        return found;
    }
    for (TextureSet* lender : m_lenders) {
        if (lender != nullptr) {
            if (auto found = find(*lender)) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::optional<TextureBinding> TextureBindings::slot(u32 index) const {
    if (index >= m_primary.size()) {
        return std::nullopt;
    }
    const auto& entry = m_primary.entry(index);
    if (!entry.external() || entry.noPicture) {
        return TextureBinding{&m_primary, index};
    }
    return image(entry.name);
}

std::optional<TextureBinding> TextureBindings::named(std::string_view name) const {
    if (const auto index = m_primary.find(name)) {
        return slot(*index);
    }
    return image(name);
}
} // namespace gdl
