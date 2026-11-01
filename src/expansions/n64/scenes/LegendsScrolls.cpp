#include "expansions/n64/scenes/LegendsScrolls.h"

#include <format>

#include "engine/core/Error.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/formats/LegendsN64Text.h"

namespace gdl::game {
std::vector<std::string> loadLegendsN64Scroll(const std::filesystem::path& directory,
                                              std::span<const u8> program, s32 number) {
    if (number <= 0 || number > 255) {
        return {};
    }
    const auto context =
        formats::legendsN64ScrollContext(program, paths::utf8(directory.filename()));
    if (!context) {
        return {};
    }
    const AssetLocator files{directory.parent_path().parent_path()};
    // e0071bc8 first tries the authored advice document, then the level's
    // indexed text bank. Do not substitute a GDL page with the same number.
    if (const auto path = files.find(
            std::format("advice/adv_{}{}_{:02}", context->realm, context->level, number))) {
        const auto bytes = readFile(*path);
        if (bytes.empty() || bytes.size() > 65536) {
            throw FormatError("N64 Legends advice has an invalid size");
        }
        std::string text;
        for (const u8 value : bytes) {
            if (value == 0) {
                break;
            }
            if (value != '\t' && value != '\r' && value != '\n' && (value < 32 || value >= 127)) {
                throw FormatError("N64 Legends advice has an unsupported encoding");
            }
            text.push_back(static_cast<char>(value));
        }
        return text.empty() ? std::vector<std::string>{}
                            : std::vector<std::string>{std::move(text)};
    }
    if (context->firstString == 0) {
        return {};
    }
    const auto library = files.find("strlib.wad");
    if (!library) {
        return {};
    }
    const auto reference = formats::legendsN64StringReference(
        readFile(*library), context->firstString + static_cast<u32>(number - 1));
    if (!reference) {
        return {};
    }
    const auto path = files.find("english/" + reference->bank + ".wad");
    if (!path) {
        return {};
    }
    auto text = formats::decodeLegendsN64Text(readFile(*path));
    if (reference->index >= text.size() || text[reference->index].empty()) {
        return {};
    }
    return {std::move(text[reference->index])};
}
} // namespace gdl::game
