#include "engine/ui/SystemFont.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

#include <stb_truetype.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep
#endif

#include "engine/core/Error.h"
#include "engine/io/File.h"

namespace gdl {
namespace {

std::filesystem::path systemFontFile() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH + 1> directory{};
    const UINT length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        throw FileError("cannot locate the Windows font directory");
    }
    const auto fonts = std::filesystem::path(directory.data()) / "Fonts";
    const std::array candidates{fonts / "segoeui.ttf", fonts / "arial.ttf", fonts / "tahoma.ttf"};
#else
    const std::array candidates{
        std::filesystem::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
        std::filesystem::path("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"),
        std::filesystem::path("/usr/share/fonts/TTF/DejaVuSans.ttf"),
        std::filesystem::path("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"),
        std::filesystem::path("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"),
        std::filesystem::path("/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf"),
        std::filesystem::path("/usr/share/fonts/TTF/LiberationSans-Regular.ttf"),
        std::filesystem::path("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"),
        std::filesystem::path("/usr/share/fonts/noto/NotoSans-Regular.ttf")};
#endif
    for (const auto& path : candidates) {
        if (std::filesystem::is_regular_file(path)) {
            return path;
        }
    }
    throw FileError("no system sans-serif font found (Segoe UI, DejaVu, Liberation or Noto Sans)");
}

struct BitmapFree {
    void operator()(u8* bitmap) const { stbtt_FreeBitmap(bitmap, nullptr); }
};

struct Glyph {
    s32 x = 0;
    s32 y = 0;
    s32 width = 0;
    s32 height = 0;
    std::unique_ptr<u8, BitmapFree> bitmap;
};

} // namespace

Image rasterizeSystemText(std::string_view text, u32 pixelHeight) {
    if (pixelHeight == 0 || pixelHeight > 128 || text.size() > 256 ||
        std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c > 126; })) {
        throw FormatError("system text requires a short ASCII line and a height of 1..128 pixels");
    }
    if (text.empty()) {
        return {};
    }
    const auto bytes = readFile(systemFontFile());
    stbtt_fontinfo font{};
    if (bytes.size() < 12 || stbtt_InitFont(&font, bytes.data(), 0) == 0) {
        throw FormatError("cannot read the installed system font");
    }
    const f32 scale = stbtt_ScaleForPixelHeight(&font, static_cast<f32>(pixelHeight));
    s32 ascent = 0;
    stbtt_GetFontVMetrics(&font, &ascent, nullptr, nullptr);
    const s32 baseline = static_cast<s32>(std::ceil(static_cast<f32>(ascent) * scale));
    f32 pen = 0;
    s32 left = 0;
    s32 top = 0;
    s32 right = 0;
    s32 bottom = static_cast<s32>(pixelHeight);
    std::vector<Glyph> glyphs;
    for (usize i = 0; i < text.size(); ++i) {
        const auto code = static_cast<unsigned char>(text[i]);
        Glyph glyph;
        glyph.bitmap.reset(stbtt_GetCodepointBitmap(&font, scale, scale, code, &glyph.width,
                                                    &glyph.height, &glyph.x, &glyph.y));
        glyph.x += static_cast<s32>(std::round(pen));
        glyph.y += baseline;
        left = std::min(left, glyph.x);
        top = std::min(top, glyph.y);
        right = std::max(right, glyph.x + glyph.width);
        bottom = std::max(bottom, glyph.y + glyph.height);
        glyphs.push_back(std::move(glyph));
        s32 advance = 0;
        stbtt_GetCodepointHMetrics(&font, code, &advance, nullptr);
        pen += static_cast<f32>(advance) * scale;
        if (i + 1 < text.size()) {
            pen += static_cast<f32>(stbtt_GetCodepointKernAdvance(
                       &font, code, static_cast<unsigned char>(text[i + 1]))) *
                   scale;
        }
    }
    right = std::max(right, static_cast<s32>(std::ceil(pen)));
    constexpr s32 kPadding = 2;
    Image image =
        Image::filled(static_cast<u32>(right - left + 2 * kPadding),
                      static_cast<u32>(bottom - top + 2 * kPadding), Color::rgba(255, 255, 255, 0));
    for (const auto& glyph : glyphs) {
        if (!glyph.bitmap) {
            continue;
        }
        for (s32 y = 0; y < glyph.height; ++y) {
            for (s32 x = 0; x < glyph.width; ++x) {
                const auto column = static_cast<u32>(glyph.x - left + kPadding + x);
                const auto row = static_cast<u32>(glyph.y - top + kPadding + y);
                const u32 coverage = glyph.bitmap.get()[y * glyph.width + x];
                const u32 previous = image.pixel(column, row).a;
                const u32 alpha = previous + coverage - previous * coverage / 255;
                image.setPixel(column, row, Color::rgba(255, 255, 255, static_cast<u8>(alpha)));
            }
        }
    }
    return image;
}

} // namespace gdl
