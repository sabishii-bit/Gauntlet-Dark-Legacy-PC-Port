#include "engine/assets/BitmapFont.h"

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

BitmapFont BitmapFont::fromGlyphs(s32 height, s32 spaceWidth, std::vector<BitmapGlyph> glyphs) {
    BitmapFont font;
    font.m_height = height;
    font.m_spaceWidth = spaceWidth;
    font.m_glyphs = std::move(glyphs);
    font.index();
    return font;
}

bool BitmapFont::load(const std::filesystem::path& file, s32 spaceWidth) {
    m_height = 0;
    m_spaceWidth = spaceWidth;
    m_glyphs.clear();
    try {
        const std::vector<u8> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        const auto height = root.at("height").get<s32>();
        for (const nlohmann::json& g : root.at("glyphs")) {
            BitmapGlyph glyph;
            glyph.code = g.at("code").get<s32>();
            glyph.width = g.at("width").get<s32>();
            glyph.x = g.at("x").get<s32>();
            glyph.y = g.at("y").get<s32>();
            m_glyphs.push_back(glyph);
        }
        if (height <= 0) {
            throw std::runtime_error("font height must be positive");
        }
        m_height = height;
    } catch (const std::exception& e) {
        log::warn("Font {}: {}", file.string(), e.what());
        m_glyphs.clear();
        m_height = 0;
        return false;
    }
    index();
    return true;
}

const BitmapGlyph* BitmapFont::glyph(u8 code) const {
    const s16 slot = m_lookup[code];
    return slot < 0 ? nullptr : &m_glyphs[static_cast<usize>(slot)];
}

void BitmapFont::index() {
    m_lookup.fill(-1);
    for (usize i = 0; i < m_glyphs.size(); ++i) {
        const s32 code = m_glyphs[i].code;
        if (code > 0 && code < static_cast<s32>(m_lookup.size()) &&
            m_lookup[static_cast<usize>(code)] < 0) {
            m_lookup[static_cast<usize>(code)] = static_cast<s16>(i);
        }
    }
}

} // namespace gdl
