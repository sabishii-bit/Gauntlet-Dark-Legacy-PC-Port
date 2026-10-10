#include <random>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/CombatReplica.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}
CombatSnapshot geometry(usize count) {
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.cameraContinuity = 1;
    state.geometry = SceneGeometry{1, static_cast<u32>(count), 0, {}};
    for (usize i = 0; i < count; ++i) {
        state.geometry->objects.push_back(
            {static_cast<u32>(i), 1, glm::translate(Mat4{1}, Vec3{static_cast<f32>(i), 5, 7})});
    }
    return state;
}

TEST_CASE("snapshot compression is exact and self-contained at every supported size",
          "[netplay][snapshot-compression]") {
    std::mt19937 random(41);
    for (const usize size : {usize{1}, usize{31}, usize{1024}, CombatPacket::kMaxBytes}) {
        for (const bool noise : {false, true}) {
            CAPTURE(size, noise);
            std::vector<u8> raw(size, 42);
            if (noise) {
                for (auto& byte : raw) {
                    byte = static_cast<u8>(random());
                }
            }
            const auto encoded = SnapshotBlock::encode(raw);
            REQUIRE(encoded);
            CHECK(encoded->size() <= raw.size() + SnapshotBlock::kHeaderBytes);
            CHECK(SnapshotBlock::decode(*encoded) == raw);
            if (noise || size == 1) {
                CHECK((*encoded)[6] == 0); // raw fallback never expands noisy data
            } else if (size >= 1024) {
                CHECK((*encoded)[6] == 1);
                CHECK(encoded->size() < raw.size() / 4);
            }
            const auto plain = SnapshotBlock::encode(raw, SnapshotBlock::Compression::None);
            REQUIRE(plain);
            CHECK(plain->size() == raw.size() + SnapshotBlock::kHeaderBytes);
            CHECK(SnapshotBlock::decode(*plain) == raw);
        }
    }
    CHECK_FALSE(SnapshotBlock::encode({}));
    CHECK_FALSE(SnapshotBlock::encode(std::vector<u8>(CombatPacket::kMaxBytes + 1)));
}

TEST_CASE("snapshot decompression rejects invalid headers lengths truncation and over-expansion",
          "[netplay][snapshot-compression]") {
    const std::vector<u8> raw(4096, 0x45);
    const auto encoded = SnapshotBlock::encode(raw);
    REQUIRE(encoded);
    REQUIRE((*encoded)[6] == 1);
    for (usize size = 0; size < encoded->size(); ++size) {
        CHECK_FALSE(SnapshotBlock::decode(std::span(*encoded).first(size)));
    }
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 8>{{{0, 0},
                                               {4, 1},
                                               {4, 0x20002},
                                               {4, 0x1000001},
                                               {8, 0},
                                               {8, static_cast<u32>(CombatPacket::kMaxBytes + 1)},
                                               {8, 4095},
                                               {8, 4097}}}) {
        auto invalid = *encoded;
        word(invalid, offset, value);
        CHECK_FALSE(SnapshotBlock::decode(invalid));
    }
    auto trailing = *encoded;
    trailing.push_back(0);
    CHECK_FALSE(SnapshotBlock::decode(trailing));
    const auto plain = SnapshotBlock::encode(raw, SnapshotBlock::Compression::None);
    REQUIRE(plain);
    auto invalid = *plain;
    invalid.pop_back();
    CHECK_FALSE(SnapshotBlock::decode(invalid));
    invalid = *plain;
    word(invalid, 4, 0x10002); // raw bytes cannot masquerade as a compressed block
    CHECK_FALSE(SnapshotBlock::decode(invalid));
    CHECK_FALSE(SnapshotBlock::decode(std::vector<u8>(SnapshotBlock::kMaxBytes + 1)));
}

TEST_CASE("byte-plane checkpoints preserve scalar bits and partial trailing words",
          "[netplay][snapshot-compression]") {
    // Unique integer low bytes previously broke LZ4's runs of identical high
    // bytes. Include non-word-aligned tails so the transform is not dependent
    // on a particular game packet layout or host byte order.
    for (usize tail = 0; tail < 4; ++tail) {
        std::vector<u8> raw(usize{4096} * 4 + tail);
        for (usize i = 0; i < 4096; ++i) {
            word(raw, i * 4, static_cast<u32>(i));
        }
        for (usize i = usize{4096} * 4; i < raw.size(); ++i) {
            raw[i] = static_cast<u8>(i);
        }
        const auto encoded = SnapshotBlock::encode(raw);
        REQUIRE(encoded);
        CHECK((*encoded)[6] == 3); // LZ4 + byte planes
        CHECK(encoded->size() < raw.size() / 8);
        CHECK(SnapshotBlock::decode(*encoded) == raw);
        for (usize size = 0; size < encoded->size(); ++size) {
            CHECK_FALSE(SnapshotBlock::decode(std::span(*encoded).first(size)));
        }
        auto invalid = *encoded;
        word(invalid, 4, 0x70002);
        CHECK_FALSE(SnapshotBlock::decode(invalid));
        word(invalid, 4, 0x20002); // planes without compression is not a supported mode
        CHECK_FALSE(SnapshotBlock::decode(invalid));
    }
}

TEST_CASE("compressed maximum checkpoints recover after loss without a prior baseline",
          "[netplay][snapshot-compression]") {
    auto state = geometry(SceneGeometry::kMaxStates);
    const auto plain = CombatReplica::packets(state, SnapshotBlock::Compression::None);
    const auto compressed = CombatReplica::packets(state);
    REQUIRE(plain);
    REQUIRE(compressed);
    REQUIRE(compressed->size() > 1);
    CHECK(compressed->size() < plain->size() / 4);
    CombatReplica replica;
    REQUIRE(replica.begin(1, 1));
    for (usize i = 1; i < compressed->size(); ++i) {
        CHECK(replica.receive(1, (*compressed)[i]) == CombatReplica::Admission::Pending);
    }
    CHECK_FALSE(replica.latest());
    ++state.motion.tick;
    state.geometry->objects[0].local[3].y = 9;
    const auto next = CombatReplica::packets(state);
    REQUIRE(next);
    for (usize i = 0; i < next->size(); ++i) {
        CHECK(replica.receive(1, (*next)[i]) == (i + 1 == next->size()
                                                     ? CombatReplica::Admission::Committed
                                                     : CombatReplica::Admission::Pending));
    }
    REQUIRE(replica.latest());
    CHECK(CombatPacket::encode(*replica.latest()) == CombatPacket::encode(state));
    CHECK(replica.receive(1, compressed->front()) == CombatReplica::Admission::Stale);
    state.motion.tick = 2;
    const auto encoded = CombatReplica::packets(state);
    REQUIRE(encoded);
    auto corrupt = *encoded;
    corrupt.back().back() ^= 1;
    for (usize i = 0; i < corrupt.size(); ++i) {
        CHECK(replica.receive(1, corrupt[i]) == (i + 1 == corrupt.size()
                                                     ? CombatReplica::Admission::Invalid
                                                     : CombatReplica::Admission::Pending));
    }
    CHECK(replica.latest()->motion.tick == 1);
}

TEST_CASE("snapshot repair recovers every individual chunk including the short final group",
          "[netplay][snapshot-compression][snapshot-repair]") {
    for (const auto compression :
         {SnapshotBlock::Compression::None, SnapshotBlock::Compression::Automatic}) {
        for (const usize size : {usize{1}, usize{17}, usize{80}, SceneGeometry::kMaxStates}) {
            const auto state = geometry(size);
            const auto data = CombatReplica::packets(state, compression);
            const auto repair =
                CombatReplica::packets(state, compression, CombatReplica::Recovery::SingleLoss);
            REQUIRE(data);
            REQUIRE(repair);
            const usize count = data->size();
            if (count == 1) {
                CHECK(*repair == *data); // no doubled overhead for tiny updates
                continue;
            }
            REQUIRE(repair->size() == count + (count + CombatReplica::kRepairGroup - 1) /
                                                  CombatReplica::kRepairGroup);
            for (usize missing = 0; missing < count; ++missing) {
                CAPTURE(size, count, missing);
                CombatReplica receiver;
                REQUIRE(receiver.begin(1, 1));
                usize commits = 0;
                for (usize i = repair->size(); i > 0; --i) {
                    if (i - 1 == missing) {
                        continue;
                    }
                    CHECK((*repair)[i - 1].size() <= PacketTransport::kMaxPacketBytes);
                    const auto result = receiver.receive(1, (*repair)[i - 1]);
                    REQUIRE(result != CombatReplica::Admission::Invalid);
                    if (result == CombatReplica::Admission::Committed) {
                        ++commits;
                    }
                }
                REQUIRE(receiver.latest());
                CHECK(commits == 1);
                CHECK(CombatPacket::encode(*receiver.latest()) == CombatPacket::encode(state));
                CHECK(receiver.receive(1, (*data)[missing]) == CombatReplica::Admission::Stale);
            }
        }
    }
}

TEST_CASE("snapshot repair bounds loss corruption duplicates and malformed parity",
          "[netplay][snapshot-compression][snapshot-repair]") {
    auto state = geometry(200);
    const auto encoded = CombatReplica::packets(state, SnapshotBlock::Compression::None,
                                                CombatReplica::Recovery::SingleLoss);
    REQUIRE(encoded);
    const usize count = encoded->front()[32] | (usize{encoded->front()[33]} << 8U);
    REQUIRE(count > 4);
    REQUIRE(encoded->size() > count);
    CombatReplica receiver;
    REQUIRE(receiver.begin(1, 1));
    SECTION("one missing data chunk per group recovers the entire checkpoint") {
        for (usize i = 0; i < encoded->size(); ++i) {
            if (i < count && i % CombatReplica::kRepairGroup == 0) {
                continue;
            }
            CHECK(receiver.receive(1, (*encoded)[i]) != CombatReplica::Admission::Invalid);
        }
        REQUIRE(receiver.latest());
        CHECK(CombatPacket::encode(*receiver.latest()) == CombatPacket::encode(state));
    }
    SECTION("two holes in a group do not commit a partially reconstructed scene") {
        for (usize i = 2; i < encoded->size(); ++i) {
            CHECK(receiver.receive(1, (*encoded)[i]) == CombatReplica::Admission::Pending);
        }
        CHECK_FALSE(receiver.latest());
        state.motion.tick = 1;
        const auto next = CombatReplica::packets(state);
        REQUIRE(next);
        for (const auto& packet : *next) {
            CHECK(receiver.receive(1, packet) != CombatReplica::Admission::Invalid);
        }
        REQUIRE(receiver.latest());
        CHECK(receiver.latest()->motion.tick == 1);
    }
    SECTION("malformed repair metadata cannot allocate an invalid assembly") {
        auto invalid = (*encoded)[count];
        invalid[34] = static_cast<u8>(encoded->size());
        CHECK(receiver.receive(1, invalid) == CombatReplica::Admission::Invalid);
        invalid = (*encoded)[count];
        invalid.pop_back();
        CHECK(receiver.receive(1, invalid) == CombatReplica::Admission::Invalid);
        CHECK(receiver.pending() == 0);
    }
    SECTION("a corrupt repair cannot pass the full checkpoint checksum") {
        auto parity = (*encoded)[count];
        parity.back() ^= 1;
        REQUIRE(receiver.receive(1, parity) == CombatReplica::Admission::Pending);
        CHECK(receiver.receive(1, parity) == CombatReplica::Admission::Duplicate);
        CHECK(receiver.receive(1, (*encoded)[count]) == CombatReplica::Admission::Invalid);
        for (usize i = 1; i < count; ++i) {
            CHECK(receiver.receive(1, (*encoded)[i]) == (i + 1 == count
                                                             ? CombatReplica::Admission::Invalid
                                                             : CombatReplica::Admission::Pending));
        }
        CHECK_FALSE(receiver.latest());
        CHECK(receiver.pending() == 0);
    }
}
} // namespace
