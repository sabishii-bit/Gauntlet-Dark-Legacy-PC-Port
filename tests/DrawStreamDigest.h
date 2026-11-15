#pragma once

#include <bit>
#include <initializer_list>
#include <span>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/io/CanonicalDigestSink.h"
#include "engine/io/Sha256.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"

namespace gdl::test {

/** Names the encoding below, so that a golden taken by another encoding cannot match. */
inline constexpr std::string_view kDrawStreamSchema = "draw-stream.1";

/** Appends a float bit for bit, so that -0.0 and +0.0 stay apart. */
inline void appendDrawnReal(CanonicalDigestSink& sink, f32 value) {
    sink.word(std::bit_cast<u32>(value));
}

inline void appendDrawnFlag(CanonicalDigestSink& sink, bool value) {
    sink.push_back(static_cast<u8>(value));
}

/** Appends a texture as a draw shows it: its size and the SHA-256 of its pixels, which two loads
 * share where their addresses differ; a mark alone for none, or for one with no pixels to read. */
inline void appendDrawnTexture(CanonicalDigestSink& sink, const Texture* texture) {
    constexpr u8 kNone = 0;
    constexpr u8 kPixels = 1;
    constexpr u8 kSizeOnly = 2;
    if (texture == nullptr) {
        sink.push_back(kNone);
        return;
    }
    const auto* image = dynamic_cast<const FakeTexture*>(texture);
    sink.push_back(image != nullptr ? kPixels : kSizeOnly);
    sink.word(texture->width());
    sink.word(texture->height());
    if (image != nullptr) {
        Sha256 pixels;
        pixels.update(image->pixels);
        sink.wide(image->pixels.size());
        sink.append(pixels.finish());
    }
}

/** Appends every field of a draw's state, its textures as appendDrawnTexture gives them. Each
 * field is bound by name, so that a field added to DrawState stops this compiling until it is
 * appended too. */
inline void appendDrawState(CanonicalDigestSink& sink, const DrawState& state) {
    const auto& [blend, lightmap, maskedTexture, nextTexture, textureBlend, uvScale, uvOffset,
                 alphaTest, cullBack, depthWrite, depthTest, mipmaps, clampTexture, lightmapCutout,
                 lightmapRgb, alphaToCoverage, smoothSprite, darken, colorScale, whiteBlend, fog] =
        state;
    sink.push_back(static_cast<u8>(blend));
    appendDrawnTexture(sink, lightmap);
    appendDrawnTexture(sink, maskedTexture);
    appendDrawnTexture(sink, nextTexture);
    for (const f32 value :
         {textureBlend, uvScale.x, uvScale.y, uvOffset.x, uvOffset.y, alphaTest}) {
        appendDrawnReal(sink, value);
    }
    for (const bool value : {cullBack, depthWrite, depthTest, mipmaps, clampTexture, lightmapCutout,
                             lightmapRgb, alphaToCoverage, smoothSprite}) {
        appendDrawnFlag(sink, value);
    }
    const auto& [fogColor, reciprocalScale, bias] = fog;
    for (const f32 value : {darken, colorScale, whiteBlend, fogColor.x, fogColor.y, fogColor.z,
                            reciprocalScale, bias}) {
        appendDrawnReal(sink, value);
    }
}

/** Appends one recorded draw: its texture, every vertex, its transform and its state. */
inline void appendRecordedDraw(CanonicalDigestSink& sink, const RecordedDraw& draw) {
    appendDrawnTexture(sink, draw.texture);
    sink.wide(draw.vertices.size());
    for (const ImmediateVertex& vertex : draw.vertices) {
        const auto& [position, color, uv, uv2] = vertex;
        for (const f32 value : {position.x, position.y, position.z}) {
            appendDrawnReal(sink, value);
        }
        for (const u8 channel : {color.r, color.g, color.b, color.a}) {
            sink.push_back(channel);
        }
        for (const f32 value : {uv.x, uv.y, uv2.x, uv2.y}) {
            appendDrawnReal(sink, value);
        }
    }
    constexpr s32 kSide = 4;
    for (s32 column = 0; column < kSide; ++column) {
        for (s32 row = 0; row < kSide; ++row) {
            appendDrawnReal(sink, draw.transform[column][row]);
        }
    }
    appendDrawState(sink, draw.state);
}

/** The SHA-256 a stream of recorded draws is pinned by: each draw, in order, bit for bit, with
 * its textures by what they hold rather than where they live. */
inline CanonicalDigestSink::Digest drawStreamDigest(std::span<const RecordedDraw> draws) {
    CanonicalDigestSink sink;
    for (const char letter : kDrawStreamSchema) {
        sink.push_back(static_cast<u8>(letter));
    }
    sink.wide(draws.size());
    for (const RecordedDraw& draw : draws) {
        appendRecordedDraw(sink, draw);
    }
    return sink.finish();
}

} // namespace gdl::test
