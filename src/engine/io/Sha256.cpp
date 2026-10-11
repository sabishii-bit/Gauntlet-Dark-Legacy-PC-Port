#include "engine/io/Sha256.h"

#include <algorithm>
#include <bit>

namespace gdl {
void Sha256::block() {
    static constexpr std::array<u32, 64> kRound{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<u32, 64> words{};
    for (usize i = 0; i < 16; ++i) {
        for (usize j = 0; j < 4; ++j) {
            words[i] = (words[i] << 8) | m_buffer[i * 4 + j];
        }
    }
    for (usize i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15];
        const auto y = words[i - 2];
        words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + words[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto [a, b, c, d, e, f, g, h] = m_state;
    for (usize i = 0; i < words.size(); ++i) {
        const u32 t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                       ((e & f) ^ (~e & g)) + kRound[i] + words[i];
        const u32 t2 =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::array<u32, 8> result{a, b, c, d, e, f, g, h};
    for (usize i = 0; i < result.size(); ++i) {
        m_state[i] += result[i];
    }
}
void Sha256::update(std::span<const u8> bytes) {
    m_size += bytes.size();
    while (!bytes.empty()) {
        const auto count = std::min(bytes.size(), m_buffer.size() - m_used);
        for (usize i = 0; i < count; ++i) {
            m_buffer[m_used + i] = bytes[i];
        }
        m_used += count;
        bytes = bytes.subspan(count);
        if (m_used == m_buffer.size()) {
            block();
            m_used = 0;
        }
    }
}
std::array<u8, 32> Sha256::finish() const {
    auto copy = *this;
    const std::array<u8, 1> marker{0x80};
    const std::array<u8, 1> zero{0};
    copy.update(marker);
    while (copy.m_used != 56) {
        copy.update(zero);
    }
    std::array<u8, 8> length{};
    for (usize i = 0; i < length.size(); ++i) {
        length[7 - i] = static_cast<u8>((m_size * 8) >> (i * 8));
    }
    copy.update(length);
    std::array<u8, 32> result{};
    for (usize i = 0; i < result.size(); ++i) {
        result[i] = static_cast<u8>(copy.m_state[i / 4] >> ((3 - i % 4) * 8));
    }
    return result;
}
std::string Sha256::hex() const {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string result;
    for (const auto byte : finish()) {
        result += kDigits[byte >> 4];
        result += kDigits[byte & 15];
    }
    return result;
}
} // namespace gdl
