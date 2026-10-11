#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/Sha256.h"

using namespace gdl;

TEST_CASE("SHA256 streaming matches published short and multiblock vectors", "[sha256][netplay]") {
    Sha256 hash;
    CHECK(hash.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hash.update(std::array<u8, 3>{'a', 'b', 'c'});
    CHECK(hash.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hash = {};
    const std::string multi = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    for (auto c : multi) {
        hash.update(std::array<u8, 1>{static_cast<u8>(c)});
    }
    CHECK(hash.hex() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    hash = {};
    const std::vector<u8> chunk(1000, 'a');
    for (usize i = 0; i < 1000; ++i) {
        hash.update(chunk);
    }
    CHECK(hash.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
