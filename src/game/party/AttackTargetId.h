#pragma once

#include <array>

#include "engine/core/Types.h"

#include "game/level/ImportedTargets.h"

namespace gdl::game {

/** Whose a party attack's target is. Never stored: only the id is. */
enum class TargetFamily : u8 {
    Barrel,
    Enemy,
    Generator,
    Critter,
    Boss,
    SafeRock,
    Wall,
    Switch,   ///< the triggers that are shot
    ItemStop, ///< chests, gates and tent walls, which stop a weapon unharmed
    Potion,   ///< the bottles lying about
    Chest,    ///< the shut chests a wave of magic reaches
    Statue,   ///< the statues a blow wakes
    Player,
    Imported, ///< an imported level's own fixtures
    None,
};

/** A party attack's target by the id the attacks offer, store and digest: its owner's family
 * and its index there. The ids are part of every rollback digest and never change. */
class AttackTargetId {
public:
    /** The ids a family owns: from `base` up to, not including, `end`. */
    struct Span {
        TargetFamily family;
        s32 base;
        s32 end;
    };
    static constexpr std::array<Span, 14> kSpans{{
        {TargetFamily::Barrel, 0, 1000},
        {TargetFamily::Enemy, 1000, 2000},
        {TargetFamily::Generator, 2000, 3000},
        {TargetFamily::Critter, 3000, 4000},
        {TargetFamily::Boss, 4000, 5000},
        {TargetFamily::SafeRock, 5000, 6000},
        {TargetFamily::Wall, 6000, 7000},
        {TargetFamily::Switch, 7000, 7500},
        {TargetFamily::ItemStop, 7500, 8000},
        {TargetFamily::Potion, 8000, 9000},
        {TargetFamily::Chest, 9000, 9500},
        {TargetFamily::Statue, 9500, 10000},
        {TargetFamily::Player, 10000, kImportedTargetBase},
        {TargetFamily::Imported, kImportedTargetBase, kImportedTargetBase + kImportedTargetCount},
    }};

    /** No target. */
    constexpr AttackTargetId() = default;

    /** Neither checks nor clamps `index`: an inert stop past the 499th takes a potion's id.
     * None names no target. */
    [[nodiscard]] static constexpr AttackTargetId of(TargetFamily family, s32 index) {
        for (const Span& span : kSpans) {
            if (span.family == family) {
                return AttackTargetId{span.base + index};
            }
        }
        return {};
    }
    [[nodiscard]] static constexpr AttackTargetId fromId(s32 id) { return AttackTargetId{id}; }
    [[nodiscard]] constexpr s32 id() const { return m_id; }
    /** The charge ledger's spelling of the id. */
    [[nodiscard]] constexpr usize key() const { return static_cast<usize>(m_id); }
    /** None below nought and past the spans. */
    [[nodiscard]] constexpr TargetFamily family() const {
        const Span* span = spanHolding(m_id);
        return span != nullptr ? span->family : TargetFamily::None;
    }
    /** Its place in its family's own roster; -1 for None. */
    [[nodiscard]] constexpr s32 index() const {
        const Span* span = spanHolding(m_id);
        return span != nullptr ? m_id - span->base : -1;
    }
    /** An enemy, a great one or a boss, not a thing. */
    [[nodiscard]] constexpr bool creature() const {
        const TargetFamily owner = family();
        return owner == TargetFamily::Enemy || owner == TargetFamily::Critter ||
               owner == TargetFamily::Boss;
    }

private:
    constexpr explicit AttackTargetId(s32 id) : m_id(id) {}
    static constexpr const Span* spanHolding(s32 id) {
        for (const Span& span : kSpans) {
            if (id >= span.base && id < span.end) {
                return &span;
            }
        }
        return nullptr;
    }
    s32 m_id = -1;
};

static_assert(
    [] {
        s32 next = 0;
        for (usize at = 0; at < AttackTargetId::kSpans.size(); ++at) {
            const AttackTargetId::Span& span = AttackTargetId::kSpans[at];
            if (span.family != static_cast<TargetFamily>(at) || span.base != next ||
                span.end <= span.base) {
                return false;
            }
            next = span.end;
        }
        return next == kImportedTargetBase + kImportedTargetCount;
    }(),
    "the spans run in family order, each starting where the last ends");

} // namespace gdl::game
