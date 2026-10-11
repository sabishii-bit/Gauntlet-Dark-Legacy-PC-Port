#pragma once

#include <cstddef>
#include <cstdint>

// Private C ABI mirrored by iroh/src/lib.rs. Opaque handles are destroyed in Rust;
// payloads are copied into caller-owned fixed-size buffers.
extern "C" {
struct GdlIroh;
struct GdlIrohEvent {
    std::uint64_t connection;
    std::uint32_t kind;
    std::uint32_t size;
    std::uint8_t bytes[1201];
};
struct GdlIrohStats {
    std::int32_t pingMs;
    std::int32_t pendingBytes;
    std::uint32_t relayed;
};
// NOLINTBEGIN(readability-identifier-naming) -- C ABI symbols match the Rust bridge.
GdlIroh* gdl_iroh_create(const std::uint8_t* invite, std::size_t length, std::uint32_t local);
void gdl_iroh_destroy(GdlIroh* handle);
std::uint32_t gdl_iroh_state(const GdlIroh* handle);
std::size_t gdl_iroh_text(const GdlIroh* handle, std::uint32_t ticket, std::uint8_t* output,
                          std::size_t capacity);
std::uint32_t gdl_iroh_poll(GdlIroh* handle, GdlIrohEvent* event);
std::uint32_t gdl_iroh_send(const GdlIroh* handle, std::uint64_t id, const std::uint8_t* bytes,
                            std::size_t length, std::uint32_t reliable);
void gdl_iroh_close(const GdlIroh* handle, std::uint64_t id);
std::uint32_t gdl_iroh_stats(const GdlIroh* handle, std::uint64_t id, GdlIrohStats* output);
// NOLINTEND(readability-identifier-naming)
}
