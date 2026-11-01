#include "expansions/shared/characters/LegendsProgress.h"

#include <algorithm>
#include <utility>

#include "engine/io/NativeStateDigest.h"

namespace gdl::game {
namespace {

u64* mask(LegendsProgress& progress, LegendsRelic kind) {
    switch (kind) {
    case LegendsRelic::Runestone: return &progress.runestones;
    case LegendsRelic::Shard: return &progress.shards;
    case LegendsRelic::Obelisk: return &progress.obelisks;
    case LegendsRelic::LegendItem: return &progress.legendItems;
    }
    return nullptr;
}

bool canonicalMapIdentity(std::string_view map) {
    return !map.empty() && map.size() <= LegendsProgress::kMaxMapName &&
           std::ranges::all_of(map, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_' ||
                      character == '-';
           });
}

} // namespace

std::optional<std::string> LegendsProgress::mapIdentity(std::string_view map) {
    if (map.empty() || map.size() > kMaxMapName) {
        return std::nullopt;
    }
    std::string result{map};
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return canonicalMapIdentity(result) ? std::optional{std::move(result)} : std::nullopt;
}

bool LegendsProgress::valid() const {
    return completedMaps.size() <= kMaxCompletedMaps &&
           std::ranges::all_of(completedMaps, canonicalMapIdentity) &&
           std::ranges::is_sorted(completedMaps) &&
           std::adjacent_find(completedMaps.begin(), completedMaps.end()) == completedMaps.end();
}

bool LegendsProgress::has(LegendsRelic kind, s32 index) const {
    if (index < 0 || index >= kMaxRelics) {
        return false;
    }
    u64 value = 0;
    switch (kind) {
    case LegendsRelic::Runestone: value = runestones; break;
    case LegendsRelic::Shard: value = shards; break;
    case LegendsRelic::Obelisk: value = obelisks; break;
    case LegendsRelic::LegendItem: value = legendItems; break;
    default: return false;
    }
    return (value & (u64{1} << static_cast<u32>(index))) != 0;
}

bool LegendsProgress::add(LegendsRelic kind, s32 index) {
    if (index < 0 || index >= kMaxRelics) {
        return false;
    }
    auto* value = mask(*this, kind);
    const u64 bit = u64{1} << static_cast<u32>(index);
    if (value == nullptr || (*value & bit) != 0) {
        return false;
    }
    *value |= bit;
    return true;
}

bool LegendsProgress::hasCompleted(std::string_view map) const {
    const auto identity = mapIdentity(map);
    return identity && std::ranges::binary_search(completedMaps, *identity);
}

bool LegendsProgress::recordCompleted(std::string_view map) {
    const auto identity = mapIdentity(map);
    if (!identity || completedMaps.size() >= kMaxCompletedMaps || !valid()) {
        return false;
    }
    const auto position = std::ranges::lower_bound(completedMaps, *identity);
    if (position != completedMaps.end() && *position == *identity) {
        return false;
    }
    completedMaps.insert(position, *identity);
    return true;
}

void visitNativeState(NativeStateDigest& digest, const LegendsProgress& value) {
    digest.fields<5>(value);
}

} // namespace gdl::game
