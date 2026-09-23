#include "engine/assets/PngImage.h"

#include <cstring>
#include <format>
#include <limits>
#include <memory>

#include <stb_image.h>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

struct StbFree {
    void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

constexpr s32 kRgbaChannels = 4;

} // namespace

Image decodeImageFile(std::span<const u8> bytes) {
    if (bytes.size() > static_cast<usize>(std::numeric_limits<s32>::max())) {
        throw FormatError("image file is too large to decode");
    }
    s32 width = 0;
    s32 height = 0;
    s32 channels = 0;
    const std::unique_ptr<stbi_uc, StbFree> pixels(stbi_load_from_memory(
        bytes.data(), static_cast<s32>(bytes.size()), &width, &height, &channels, kRgbaChannels));
    if (!pixels || width <= 0 || height <= 0) {
        throw FormatError(std::format("cannot decode image: {}", stbi_failure_reason()));
    }
    Image image;
    image.width = static_cast<u32>(width);
    image.height = static_cast<u32>(height);
    image.pixels.resize(image.rowBytes() * image.height);
    std::memcpy(image.pixels.data(), pixels.get(), image.pixels.size());
    return image;
}

Image loadImageFile(const std::filesystem::path& path) {
    return decodeImageFile(readFile(path));
}

} // namespace gdl
