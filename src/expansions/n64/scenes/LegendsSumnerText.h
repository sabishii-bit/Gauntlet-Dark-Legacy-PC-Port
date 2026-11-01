#pragma once

#include <string>

#include "engine/core/Types.h"

namespace gdl::game {

/** Localized key for a verified native Sumner string identity, or empty when
 * it is not in the catalog. Does not open optional expansion assets. */
std::string legendsN64SumnerTextKey(u32 identity);

/** Localized external advice/adv_t1_## document. Only authored pages have a
 * key; absent pages must not fall back to unrelated indexed ceremony text. */
std::string legendsN64SumnerHistoryKey(u8 page);

} // namespace gdl::game
