#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

struct LegendsN64ScrollContext {
    char realm = 0;
    u32 level = 0;
    u16 firstString = 0;
};
struct LegendsN64StringReference {
    std::string bank;
    u32 index = 0;
};

std::optional<LegendsN64ScrollContext> legendsN64ScrollContext(std::span<const u8> program,
                                                               std::string_view map);
std::optional<LegendsN64StringReference> legendsN64StringReference(std::span<const u8> library,
                                                                   u32 identity);
std::vector<std::string> decodeLegendsN64Text(std::span<const u8> bytes);

} // namespace gdl::formats
