#pragma once

#include "engine/core/Types.h"

namespace gdl::game {
/** Where a completed secret challenge's classes are granted, even before saving. */
class ClassUnlocks {
public:
    virtual ~ClassUnlocks() = default;
    /** Makes the classes of `mask` available to every character, bit by bit as
     * `CharacterSave::classUnlock` keeps them. */
    virtual void unlockClasses(u16 mask) = 0;
};
} // namespace gdl::game
