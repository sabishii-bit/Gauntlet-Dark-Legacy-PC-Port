#include "engine/assets/PngImage.h"

#include <cstring>
#include <format>
#include <limits>
#include <memory>

#include <stb_image.h>

#include "engine/core/Error.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

struct StbFree {
    void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

constexpr int kRgbaChannels = 4;

} // namespace

Image decodeImageFile(std::span<const u8> bytes) {
    if (bytes.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        throw FormatError("image file is too large to decode");
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::unique_ptr<stbi_uc, StbFree> pixels(stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, kRgbaChannels));
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
