#pragma once

#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"

namespace gdl::game {

/** What reading a binding list does with a name it does not know. */
enum class UnknownBinding : u8 {
    WarnAndSkip, ///< warns and reads on: the bindings every player shares
    Refuse       ///< throws FormatError: a player's own profile
};

/** Reads a list of key names into `keys`, keeping Escape only when `keepsEscape`. `what` is the
 * binding's name in the file, which a warning gives. A lone name reads as a list of it, null as
 * none and an object as its values; a name that is not a string throws nlohmann's type_error,
 * leaving `keys` with the names read before it. */
void readKeyNames(const nlohmann::json& list, std::string_view what, UnknownBinding policy,
                  bool keepsEscape, std::vector<Key>& keys);

/** Reads a list of pad button names into `buttons`, as readKeyNames reads keys. */
void readButtonNames(const nlohmann::json& list, std::string_view what, UnknownBinding policy,
                     std::vector<PadButton>& buttons);

} // namespace gdl::game
