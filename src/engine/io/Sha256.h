#pragma once

#include <array>
#include <span>
#include <string>

#include "engine/core/Types.h"

namespace gdl {
/** Streaming SHA-256 for content compatibility checks (not authentication). */
class Sha256 {
public:
    void update(std::span<const u8> bytes);
    std::array<u8, 32> finish() const;
    std::string hex() const;

private:
    void block();
    std::array<u32, 8> m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<u8, 64> m_buffer{};
    u64 m_size = 0;
    usize m_used = 0;
};
} // namespace gdl
