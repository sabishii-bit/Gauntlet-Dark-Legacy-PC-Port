#include "game/widgets/MenuFont.h"

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {
constexpr std::string_view kMenuFontFile = "fonts/font32.json";
} // namespace

bool loadMenuFont(BitmapFont& font, const std::filesystem::path& unpackedRoot, s32 spaceWidth) {
    if (!font.load(unpackedRoot / kMenuFontFile, spaceWidth)) {
        return false;
    }
    if (font.glyph('-') == nullptr && !font.addCut(kMenuFontHyphen)) {
        log::warn("The menu font has no E to cut a hyphen from; hyphens will not show");
    }
    return true;
}

} // namespace gdl::game
