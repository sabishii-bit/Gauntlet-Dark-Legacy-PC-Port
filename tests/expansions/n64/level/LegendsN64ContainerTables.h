#pragma once

#include <bit>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/n64/level/LegendsContainers.h"
#include "expansions/shared/level/LegendsContainers.h"

namespace gdl::test {

inline void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes[at] = static_cast<u8>(value >> 8U);
    bytes[at + 1] = static_cast<u8>(value);
}
inline void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
inline void putFloat(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}

/** A native PCon barrel template holding a PPower treasure, and its placement;
 * `chest` turns them into a locked chest. */
struct N64ContainerTables {
    formats::LegendsN64Fixture fixture;
    std::vector<u8> containers = std::vector<u8>(48);
    std::vector<u8> powerups = std::vector<u8>(32);
    N64ContainerTables() {
        fixture.kind = formats::LegendsN64FixtureKind::Container;
        fixture.templateKey = 0x1901;
        fixture.nativeIndex = 7;
        fixture.position = {5, 2, 9};
        fixture.facingRadians = .7f;
        fixture.containedPowerup = 0x0301;
        put32(containers, 0, 1);
        put16(containers, 4, fixture.templateKey);
        put16(containers, 6, 6);
        put32(containers, 8, 0x24000000);
        put32(containers, 12, 0x43425243); // CBRC
        put32(containers, 16, 0x43425241); // CBRA
        put32(containers, 20, 0x43425244); // CBRD
        put32(containers, 28, 1);
        putFloat(containers, 32, 1);
        putFloat(containers, 36, 3);
        put32(containers, 44, 22);
        put32(powerups, 0, 1);
        put16(powerups, 4, 0x0301);
        put16(powerups, 6, 150);
        put32(powerups, 8, 6);
        put32(powerups, 12, 0x54525331); // TRS1
    }
    void chest() {
        fixture.templateKey = 0x1702;
        put16(containers, 4, fixture.templateKey);
        put16(containers, 6, 0);
        put32(containers, 8, 0x02000000);
        put32(containers, 12, 0x43434743); // CCGC
        put32(containers, 16, 0x43434741); // CCGA
        put32(containers, 20, 0);
        put32(containers, 28, 3);
        putFloat(containers, 32, 1.2f);
        putFloat(containers, 36, 1);
        putFloat(containers, 40, 1.2f);
        put32(containers, 44, 29);
    }
    game::LegendsN64Container decodeN64() const {
        const auto result = game::decodeLegendsN64Container(fixture, containers, powerups);
        REQUIRE(result);
        return *result;
    }
    game::LegendsContainerDefinition decode() const { return decodeN64().definition; }
};

} // namespace gdl::test
