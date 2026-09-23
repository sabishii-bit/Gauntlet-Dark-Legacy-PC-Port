#include "engine/assets/BitmapFont.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

BitmapFont BitmapFont::fromGlyphs(std::int32_t height, std::int32_t spaceWidth,
                                  std::vector<BitmapGlyph> glyphs) {
    BitmapFont font;
    font.m_height = height;
    font.m_spaceWidth = spaceWidth;
    font.m_glyphs = std::move(glyphs);
    font.index();
    return font;
}

bool BitmapFont::load(const std::filesystem::path& file, std::int32_t spaceWidth) {
    m_height = 0;
    m_spaceWidth = spaceWidth;
    m_glyphs.clear();
    try {
        const std::vector<std::uint8_t> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        const auto height = root.at("height").get<std::int32_t>();
        for (const nlohmann::json& g : root.at("glyphs")) {
            BitmapGlyph glyph;
            glyph.code = g.at("code").get<std::int32_t>();
            glyph.width = g.at("width").get<std::int32_t>();
            glyph.x = g.at("x").get<std::int32_t>();
            glyph.y = g.at("y").get<std::int32_t>();
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

const BitmapGlyph* BitmapFont::glyph(std::uint8_t code) const {
    const std::int16_t slot = m_lookup[code];
    return slot < 0 ? nullptr : &m_glyphs[static_cast<std::size_t>(slot)];
}

void BitmapFont::index() {
    m_lookup.fill(-1);
    for (std::size_t i = 0; i < m_glyphs.size(); ++i) {
        const std::int32_t code = m_glyphs[i].code;
        if (code > 0 && code < static_cast<std::int32_t>(m_lookup.size()) &&
            m_lookup[static_cast<std::size_t>(code)] < 0) {
            m_lookup[static_cast<std::size_t>(code)] = static_cast<std::int16_t>(i);
        }
    }
}

} // namespace gdl
