#include "formats/TextRom.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kLumpHeaderSize = 16;
constexpr std::size_t kFontEntrySize = 20;
constexpr std::size_t kMessageEntrySize = 20;
constexpr std::size_t kListEntrySize = 8;

/** Lump ids are stored as the big-endian packing of their four letters. */
constexpr std::uint32_t lumpId(std::string_view code) {
    return (std::uint32_t{static_cast<std::uint8_t>(code[0])} << 24U) |
           (std::uint32_t{static_cast<std::uint8_t>(code[1])} << 16U) |
           (std::uint32_t{static_cast<std::uint8_t>(code[2])} << 8U) |
           std::uint32_t{static_cast<std::uint8_t>(code[3])};
}

constexpr std::uint32_t kFonts = lumpId("FONT");
constexpr std::uint32_t kText = lumpId("TEXT");
constexpr std::uint32_t kTextOffsets = lumpId("TOFF");
constexpr std::uint32_t kMessages = lumpId("STRS");
constexpr std::uint32_t kListIndices = lumpId("LOFF");
constexpr std::uint32_t kLists = lumpId("LIST");
constexpr std::uint32_t kNames = lumpId("DEFS");
constexpr std::uint32_t kMessageNames = lumpId("SDEF");
constexpr std::uint32_t kListNames = lumpId("LDEF");

struct Lump {
    std::uint32_t id = 0;
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    std::size_t bytes = 0;
};

std::string readCString(std::span<const std::uint8_t> blob, std::size_t offset) {
    std::string text;
    while (offset < blob.size() && blob[offset] != 0) {
        text.push_back(static_cast<char>(blob[offset++]));
    }
    return text;
}

std::vector<std::uint32_t> readU32Table(std::span<const std::uint8_t> file, const Lump& lump,
                                        bool countIsEntries) {
    const std::size_t entries = countIsEntries ? lump.count : lump.bytes / 4;
    std::vector<std::uint32_t> values;
    values.reserve(entries);
    for (std::size_t i = 0; i < entries; ++i) {
        values.push_back(readU32LE(file, lump.offset + i * 4));
    }
    return values;
}

} // namespace

TextRom TextRom::parse(std::span<const std::uint8_t> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("text rom is too small for its header");
    }
    const std::uint32_t tableOffset = readU32LE(file, 0);
    const std::uint32_t lumpCount = readU32LE(file, 4);
    if (tableOffset > file.size() ||
        std::size_t{lumpCount} * kLumpHeaderSize > file.size() - tableOffset) {
        throw FormatError("text rom lump table lies outside the file");
    }

    std::vector<Lump> lumps;
    for (std::uint32_t i = 0; i < lumpCount; ++i) {
        const std::size_t at = tableOffset + std::size_t{i} * kLumpHeaderSize;
        Lump lump;
        lump.id = readU32LE(file, at);
        lump.offset = readU32LE(file, at + 4);
        lump.count = readU32LE(file, at + 8);
        if (lump.offset > tableOffset) {
            throw FormatError("text rom lump lies outside the file");
        }
        lumps.push_back(lump);
    }
    std::vector<std::uint32_t> ends;
    ends.reserve(lumps.size() + 1);
    for (const Lump& lump : lumps) {
        ends.push_back(lump.offset);
    }
    ends.push_back(tableOffset);
    std::ranges::sort(ends);
    for (Lump& lump : lumps) {
        const auto next = std::ranges::upper_bound(ends, lump.offset);
        lump.bytes = (next == ends.end() ? tableOffset : *next) - lump.offset;
    }

    const auto find = [&lumps](std::uint32_t id) -> const Lump* {
        const auto it = std::ranges::find_if(lumps, [id](const Lump& l) { return l.id == id; });
        return it == lumps.end() ? nullptr : &*it;
    };

    TextRom rom;
    if (const Lump* fonts = find(kFonts)) {
        for (std::uint32_t i = 0; i < fonts->count; ++i) {
            const std::size_t at = fonts->offset + std::size_t{i} * kFontEntrySize;
            if (at + kFontEntrySize > file.size()) {
                throw FormatError("text rom font table is truncated");
            }
            rom.fonts.push_back(readCString(file.subspan(at, 16), 0));
        }
    }

    std::vector<std::string> strings;
    const Lump* text = find(kText);
    const Lump* textOffsets = find(kTextOffsets);
    if (text != nullptr && textOffsets != nullptr) {
        const std::span<const std::uint8_t> blob = file.subspan(text->offset, text->bytes);
        for (const std::uint32_t offset : readU32Table(file, *textOffsets, false)) {
            strings.push_back(offset < blob.size() ? readCString(blob, offset) : std::string{});
        }
    }

    std::vector<std::string> messageNames;
    std::vector<std::string> listNames;
    if (const Lump* names = find(kNames)) {
        const std::span<const std::uint8_t> blob = file.subspan(names->offset, names->bytes);
        if (const Lump* offsets = find(kMessageNames)) {
            for (const std::uint32_t offset : readU32Table(file, *offsets, true)) {
                messageNames.push_back(readCString(blob, offset));
            }
        }
        if (const Lump* offsets = find(kListNames)) {
            for (const std::uint32_t offset : readU32Table(file, *offsets, true)) {
                listNames.push_back(readCString(blob, offset));
            }
        }
    }

    if (const Lump* messages = find(kMessages)) {
        for (std::uint32_t i = 0; i < messages->count; ++i) {
            const std::size_t at = messages->offset + std::size_t{i} * kMessageEntrySize;
            if (at + kMessageEntrySize > file.size()) {
                throw FormatError("text rom message table is truncated");
            }
            TextMessage message;
            const auto count = static_cast<std::int32_t>(readU32LE(file, at));
            const auto first = static_cast<std::int32_t>(readU32LE(file, at + 4));
            message.font = static_cast<std::int32_t>(readU32LE(file, at + 8));
            message.scale = std::bit_cast<float>(readU32LE(file, at + 12));
            message.shadowScale = std::bit_cast<float>(readU32LE(file, at + 16));
            for (std::int32_t k = 0; k < count; ++k) {
                const std::size_t index =
                    static_cast<std::size_t>(first) + static_cast<std::size_t>(k);
                message.lines.push_back(index < strings.size() ? strings[index] : std::string{});
            }
            if (i < messageNames.size()) {
                message.name = normalizeAssetName(messageNames[i]);
            }
            rom.messages.push_back(std::move(message));
        }
    }

    const Lump* lists = find(kLists);
    const Lump* listIndices = find(kListIndices);
    if (lists != nullptr && listIndices != nullptr) {
        const std::vector<std::uint32_t> indices = readU32Table(file, *listIndices, false);
        for (std::uint32_t i = 0; i < lists->count; ++i) {
            const std::size_t at = lists->offset + std::size_t{i} * kListEntrySize;
            if (at + kListEntrySize > file.size()) {
                throw FormatError("text rom list table is truncated");
            }
            TextMessageList list;
            const auto count = static_cast<std::int32_t>(readU32LE(file, at));
            const auto first = static_cast<std::int32_t>(readU32LE(file, at + 4));
            for (std::int32_t k = 0; k < count; ++k) {
                const std::size_t index =
                    static_cast<std::size_t>(first) + static_cast<std::size_t>(k);
                if (index < indices.size()) {
                    list.messages.push_back(indices[index]);
                }
            }
            if (i < listNames.size()) {
                list.name = normalizeAssetName(listNames[i]);
            }
            rom.lists.push_back(std::move(list));
        }
    }
    return rom;
}

std::optional<std::size_t> TextRom::findMessage(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].name == key) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> TextRom::findList(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (std::size_t i = 0; i < lists.size(); ++i) {
        if (lists[i].name == key) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
