#include "game/mechanics/MechanicCatalog.h"

#include <algorithm>

namespace gdl::game {
namespace {

/** Lowercase letters, digits and dashes: a settings key, a folder and a digest name alike. */
bool validId(std::string_view id) {
    return !id.empty() && id.size() <= MechanicCatalog::kMostIdBytes &&
           std::ranges::all_of(id, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}

} // namespace

bool MechanicCatalog::add(const MechanicDescriptor& descriptor) {
    if (!validId(descriptor.id) || descriptor.label.empty() || descriptor.create == nullptr ||
        find(descriptor.id) != nullptr ||
        (descriptor.roomRule && roomRules().size() >= kMostRoomRules)) {
        return false;
    }
    const auto at =
        std::ranges::upper_bound(m_descriptors, descriptor.id, {}, &MechanicDescriptor::id);
    m_descriptors.insert(at, descriptor);
    return true;
}

const MechanicDescriptor* MechanicCatalog::find(std::string_view id) const {
    const auto found = std::ranges::find(m_descriptors, id, &MechanicDescriptor::id);
    return found != m_descriptors.end() ? &*found : nullptr;
}

std::vector<const MechanicDescriptor*> MechanicCatalog::roomRules() const {
    std::vector<const MechanicDescriptor*> rules;
    for (const MechanicDescriptor& descriptor : m_descriptors) {
        if (descriptor.roomRule) {
            rules.push_back(&descriptor);
        }
    }
    return rules;
}

u8 MechanicCatalog::knownRoomBits() const {
    return static_cast<u8>((1U << roomRules().size()) - 1U);
}

u8 MechanicCatalog::roomBits(const GameOptionsConfig& options) const {
    u8 bits = 0;
    const auto rules = roomRules();
    for (usize bit = 0; bit < rules.size(); ++bit) {
        if (enabled(options, rules[bit]->id)) {
            bits = static_cast<u8>(bits | (1U << bit));
        }
    }
    return bits;
}

bool MechanicCatalog::enabled(const GameOptionsConfig& options, std::string_view id) {
    const auto found = options.mechanics.find(id);
    return found != options.mechanics.end() && found->second;
}

std::string MechanicCatalog::roster() const {
    std::string text;
    for (const MechanicDescriptor& descriptor : m_descriptors) {
        text.append(descriptor.id);
        text.push_back('@');
        text.append(std::to_string(descriptor.schema));
        text.push_back('\n');
    }
    return text;
}

void applyRoomRules(const MechanicCatalog* catalog, u8 bits, GameOptionsConfig& options) {
    options.mechanics.clear();
    if (catalog == nullptr) {
        return;
    }
    const auto rules = catalog->roomRules();
    for (usize bit = 0; bit < rules.size(); ++bit) {
        options.mechanics[std::string{rules[bit]->id}] = ((bits >> bit) & 1U) != 0;
    }
}

} // namespace gdl::game
