#include <cstring>
#include <chrono>
#include <iostream>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

namespace {
using namespace gdl;

template <class T>
concept DirectlyDigestible =
    requires(NativeStateDigest& digest, const T& value) { digest.value(value); };
static_assert(!DirectlyDigestible<int*>);
static_assert(!DirectlyDigestible<const void*>);

template <class T> auto hash(const T& value) {
    NativeStateDigest digest;
    digest.value(value);
    REQUIRE(digest.good());
    return digest.finish();
}

struct Value {
    u8 flag = 0;
    f64 time = 0;
    std::vector<s32> pending;
};
void visitNativeState(NativeStateDigest& digest, const Value& value) {
    digest.fields<3>(value);
}

TEST_CASE("native digest follows actual fields not allocation or padding",
          "[native-state-digest]") {
    Value first{4, 0.5, {9, 8, 7}};
    Value second = first;
    second.pending.reserve(128);
    REQUIRE(first.pending.data() != second.pending.data());
    CHECK(hash(first) == hash(second));
    second.pending.back() = 6;
    CHECK(hash(first) != hash(second));
    second = first;
    second.time += 0.001;
    CHECK(hash(first) != hash(second));

    struct Padded {
        u8 code;
        f64 time;
    };
    static_assert(std::is_trivially_copyable_v<Padded>);
    Padded a{};
    Padded b{};
    std::memset(&a, 0xAA, sizeof(a));
    std::memset(&b, 0x55, sizeof(b));
    a.code = b.code = 2;
    a.time = b.time = 0.25;
    NativeStateDigest left;
    NativeStateDigest right;
    left.fields<2>(a);
    right.fields<2>(b);
    CHECK(left.finish() == right.finish());
}

TEST_CASE("native digest canonicalizes unordered containers without losing contents",
          "[native-state-digest]") {
    std::unordered_map<s32, std::vector<s32>> const first{{2, {9, 8}}, {5, {1, 3}}};
    std::unordered_map<s32, std::vector<s32>> second;
    second.reserve(128);
    second.emplace(5, std::vector<s32>{1, 3});
    second.emplace(2, std::vector<s32>{9, 8});
    CHECK(hash(first) == hash(second));
    second.at(5).back() = 4;
    CHECK(hash(first) != hash(second));
    CHECK(hash(std::unordered_set<s32>{1, 2}) == hash(std::unordered_set<s32>{2, 1}));
    CHECK(hash(std::unordered_set<s32>{1, 2}) != hash(std::unordered_set<s32>{1, 3}));
    CHECK(hash(std::optional<s32>{}) != hash(std::optional<s32>{0}));
    CHECK(hash(std::vector<bool>{false, true}) != hash(std::vector<bool>{true, false}));
}

TEST_CASE("native digest includes RNG state without advancing it", "[native-state-digest]") {
    Mt19937 first{71};
    const auto saved = first;
    const auto before = hash(first);
    CHECK(first == saved);
    first();
    CHECK(hash(first) != before);
    first = saved;
    CHECK(hash(first) == before);
    std::minstd_rand linear{31};
    const auto savedLinear = linear;
    const auto beforeLinear = hash(linear);
    CHECK(linear == savedLinear);
    linear();
    CHECK(hash(linear) != beforeLinear);

    std::mt19937 standard{71};
    const auto savedStandard = standard;
    const auto beforeStandard = hash(standard);
    CHECK(standard == savedStandard);
    for (u32 const count : {1u, 623u, 624u, 625u}) {
        standard = savedStandard;
        standard.discard(count);
        CHECK(hash(standard) != beforeStandard);
        const auto untouched = standard;
        CHECK(hash(standard) == hash(untouched));
        CHECK(standard == untouched);
    }
    standard = savedStandard;
    CHECK(hash(standard) == beforeStandard);
}

TEST_CASE("a standard engine is hashed by the outputs it will give, which every library shares",
          "[native-state-digest]") {
    // Libraries stream an engine differently (a cursor, or a rotated history), but the
    // standard fixes its outputs; past a twist is where the layouts part.
    std::mt19937 standard{71};
    standard.discard(1000);
    NativeStateDigest outputs;
    auto copy = standard;
    for (usize i = 0; i < std::mt19937::state_size; ++i) {
        outputs.word(static_cast<u32>(copy()));
    }
    REQUIRE(outputs.good());
    CHECK(hash(standard) == outputs.finish());
}

TEST_CASE("a linear congruential engine is hashed at its modulus's width on every library",
          "[native-state-digest]") {
    // std::minstd_rand's result type is four bytes on one library and eight on another; its
    // outputs, below 2^31, are hashed as four everywhere.
    std::minstd_rand engine{31};
    engine.discard(5);
    NativeStateDigest expected;
    auto copy = engine;
    expected.word(static_cast<u32>(copy()));
    REQUIRE(expected.good());
    CHECK(hash(engine) == expected.finish());
}

struct Borrower {
    const s32* resource = nullptr;
};
void visitNativeState(NativeStateDigest& digest, const Borrower& value) {
    const auto& [resource] = value;
    digest.resource(resource);
}

TEST_CASE("native digest refuses unresolved resources including nested unordered values",
          "[native-state-digest]") {
    const s32 first = 8;
    const s32 second = 8;
    NativeStateDigest missing;
    missing.resource(&first);
    CHECK_FALSE(missing.checkedFinish());
    const NativeStateDigest::Resources bindings{
        .append = [](NativeStateDigest& sink, std::type_index type, const void* pointer, void*) {
            if (type != typeid(s32)) {
                return false;
            }
            sink.string("fixture model slot");
            sink.value(*static_cast<const s32*>(pointer));
            return true;
        }};
    NativeStateDigest left{bindings};
    NativeStateDigest right{bindings};
    left.resource(&first);
    right.resource(&second);
    REQUIRE(left.checkedFinish());
    CHECK(left.checkedFinish() == right.checkedFinish());
    right.resource(static_cast<const s32*>(nullptr));
    CHECK(left.checkedFinish() != right.checkedFinish());
    NativeStateDigest nested;
    nested.value(std::unordered_map<s32, Borrower>{{2, {&first}}});
    CHECK_FALSE(nested.checkedFinish());
    NativeStateDigest empty;
    empty.resource(static_cast<const s32*>(nullptr));
    CHECK(empty.checkedFinish());
}

TEST_CASE("native digest scalar and string stream preserves its original canonical bytes",
          "[native-state-digest][canonical-digest]") {
    NativeStateDigest actual;
    std::vector<u8> bytes;
    const auto word = [&](u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<u8>(value >> shift));
        }
    };
    const auto wide = [&](u64 value) {
        word(static_cast<u32>(value));
        word(static_cast<u32>(value >> 32));
    };
    for (u32 index = 0; index < 800; ++index) {
        const u32 bits = index * 0x1020304U;
        actual.word(bits);
        word(bits);
        const u64 large = (static_cast<u64>(~bits) << 32) | bits;
        actual.wide(large);
        wide(large);
        // Include embedded zero/high bytes and lengths on both sides of the
        // sink boundary. Text is bytes, never locale conversion or C strings.
        std::string text(index % 71, '\0');
        for (usize at = 0; at < text.size(); ++at) {
            text[at] = static_cast<char>(at * 73 + index);
        }
        actual.string(text);
        wide(text.size());
        for (const char byte : text) {
            bytes.push_back(static_cast<u8>(byte));
        }
    }
    Sha256 original;
    original.update(bytes);
    REQUIRE(actual.good());
    CHECK(actual.finish() == original.finish());
}

TEST_CASE("native digest unordered elements retain their original independently sorted SHA stream",
          "[native-state-digest][canonical-digest]") {
    std::unordered_map<s32, std::vector<s32>> values;
    for (s32 index = -32; index <= 32; ++index) {
        values.emplace(index, std::vector<s32>{index * 17, -index, 0});
    }
    std::vector<CanonicalDigestSink::Digest> elements;
    const auto word = [](CanonicalDigestSink& sink, u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            sink.push_back(static_cast<u8>(value >> shift));
        }
    };
    for (const auto& [key, payload] : values) {
        CanonicalDigestSink element;
        word(element, static_cast<u32>(key));
        word(element, static_cast<u32>(payload.size()));
        word(element, 0);
        for (const auto value : payload) {
            word(element, static_cast<u32>(value));
        }
        elements.push_back(element.finish());
    }
    std::ranges::sort(elements);
    CanonicalDigestSink original;
    word(original, static_cast<u32>(elements.size()));
    word(original, 0);
    for (const auto& element : elements) {
        original.append(element);
    }
    CHECK(hash(values) == original.finish());
}

TEST_CASE("native scalar digest reports bytewise and fixed-width costs",
          "[.native-digest-benchmark][native-state-digest]") {
    std::array<double, 16> bytewise{};
    std::array<double, 16> fixed{};
    const auto run = [](bool bulk) {
        CanonicalDigestSink sink;
        for (u64 index = 0; index < 32768; ++index) {
            const u32 word = static_cast<u32>(index * 1234567);
            const u64 wide = index * 0x123456789abcdefULL;
            if (bulk) {
                sink.word(word);
                sink.wide(wide);
            } else {
                for (u32 shift = 0; shift < 32; shift += 8) {
                    sink.push_back(static_cast<u8>(word >> shift));
                }
                for (u32 shift = 0; shift < 64; shift += 8) {
                    sink.push_back(static_cast<u8>(wide >> shift));
                }
            }
        }
        return sink.finish();
    };
    const auto expected = run(false);
    REQUIRE(run(true) == expected);
    for (usize sample = 0; sample < bytewise.size(); ++sample) {
        // Alternate order so neither route always receives the warmer CPU.
        for (usize phase = 0; phase < 2; ++phase) {
            const bool bulk = (sample + phase) % 2 != 0;
            const auto start = std::chrono::steady_clock::now();
            const auto result = run(bulk);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            (bulk ? fixed : bytewise)[sample] = elapsed;
            REQUIRE(result == expected);
        }
    }
    std::ranges::sort(bytewise);
    std::ranges::sort(fixed);
    std::cout << "native canonical scalar bytes=393216 old-bytewise-p50-ms=" << bytewise[8]
              << " fixed-width-p50-ms=" << fixed[8] << '\n';
}
} // namespace
