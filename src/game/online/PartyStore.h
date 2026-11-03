#pragma once

#include <span>

#include "game/characters/Party.h"

namespace gdl::game {
/** Saves a party's locally owned characters. */
class PartyStore {
public:
    virtual ~PartyStore() = default;
    /** A checkpoint's save, after a shop or a won challenge; Auto Save decides whether it is
     * written. */
    virtual void autoSave(std::span<PartyMember> party) = 0;
    /** An explicit request; may assign free slots; false reports an incomplete save. */
    virtual bool saveParty(std::span<PartyMember> party) = 0;
};
} // namespace gdl::game
