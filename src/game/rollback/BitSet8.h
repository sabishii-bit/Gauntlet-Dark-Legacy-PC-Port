#pragma once

#include <bit>

#include "engine/core/Types.h"

namespace gdl::game {

/** A set of indices below eight held as the one byte of bits a packet carries, a bit an
 * index. `Tag` keeps sets of different things apart: a peer set never passes for a seat
 * set. Indices past seven are never held. */
template <class Tag> class BitSet8 {
public:
    static constexpr usize kCapacity = 8;

    constexpr BitSet8() = default;
    /** The set the byte `bits` spells, as read from a packet. */
    static constexpr BitSet8 fromBits(u8 bits) {
        BitSet8 set;
        set.m_bits = bits;
        return set;
    }
    /** The set holding `index` alone, or nothing when it is past the capacity. */
    static constexpr BitSet8 of(usize index) {
        BitSet8 set;
        set.insert(index);
        return set;
    }

    /** The byte a packet carries. */
    constexpr u8 bits() const { return m_bits; }
    constexpr bool empty() const { return m_bits == 0; }
    constexpr usize count() const { return static_cast<usize>(std::popcount(m_bits)); }
    constexpr bool contains(usize index) const {
        return index < kCapacity && (static_cast<u32>(m_bits) & bit(index)) != 0;
    }
    /** Whether every index of `other` is in this set. */
    constexpr bool includes(BitSet8 other) const {
        return (static_cast<u32>(other.m_bits) & ~static_cast<u32>(m_bits)) == 0;
    }

    constexpr void insert(usize index) {
        if (index < kCapacity) {
            m_bits = static_cast<u8>(static_cast<u32>(m_bits) | bit(index));
        }
    }
    constexpr void insert(BitSet8 other) {
        m_bits = static_cast<u8>(static_cast<u32>(m_bits) | other.m_bits);
    }
    constexpr void erase(usize index) {
        if (index < kCapacity) {
            m_bits = static_cast<u8>(static_cast<u32>(m_bits) & ~bit(index));
        }
    }
    constexpr void erase(BitSet8 other) {
        m_bits = static_cast<u8>(static_cast<u32>(m_bits) & ~static_cast<u32>(other.m_bits));
    }

    bool operator==(const BitSet8&) const = default;
    /** A digest takes the set as the byte it is. */
    template <class Visitor> friend void visitNativeState(Visitor& visitor, const BitSet8& set) {
        visitor.value(set.m_bits);
    }

private:
    static constexpr u32 bit(usize index) { return 1U << index; }

    u8 m_bits = 0;
};

struct PeerSetTag {};
struct SeatSetTag {};
/** Machines in a room by peer id, 1 the host and 2 to 4 its guests. */
using PeerSet = BitSet8<PeerSetTag>;
/** Places in the party by seat, 0 to 3, whichever machine plays them. */
using SeatSet = BitSet8<SeatSetTag>;

} // namespace gdl::game
