#include "formats/TextRom.h"

#include <algorithm>
#include <bit>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 8;
constexpr usize kLumpHeaderSize = 16;
constexpr usize kFontEntrySize = 20;
constexpr usize kMessageEntrySize = 20;
constexpr usize kListEntrySize = 8;

/** Lump ids are stored as the big-endian packing of their four letters. */
constexpr u32 lumpId(std::string_view code) {
    return (u32{static_cast<u8>(code[0])} << 24U) | (u32{static_cast<u8>(code[1])} << 16U) |
           (u32{static_cast<u8>(code[2])} << 8U) | u32{static_cast<u8>(code[3])};
}

constexpr u32 kFonts = lumpId("FONT");
constexpr u32 kText = lumpId("TEXT");
constexpr u32 kTextOffsets = lumpId("TOFF");
constexpr u32 kMessages = lumpId("STRS");
constexpr u32 kListIndices = lumpId("LOFF");
constexpr u32 kLists = lumpId("LIST");
constexpr u32 kNames = lumpId("DEFS");
constexpr u32 kMessageNames = lumpId("SDEF");
constexpr u32 kListNames = lumpId("LDEF");

struct Lump {
    u32 id = 0;
    u32 offset = 0;
    u32 count = 0;
    usize bytes = 0;
};

std::string readCString(std::span<const u8> blob, usize offset) {
    std::string text;
    while (offset < blob.size() && blob[offset] != 0) {
        text.push_back(static_cast<char>(blob[offset++]));
    }
    return text;
}

std::vector<u32> readU32Table(std::span<const u8> file, const Lump& lump, bool countIsEntries) {
    const usize entries = countIsEntries ? lump.count : lump.bytes / 4;
    std::vector<u32> values;
    values.reserve(entries);
    for (usize i = 0; i < entries; ++i) {
        values.push_back(readU32LE(file, lump.offset + i * 4));
    }
    return values;
}

} // namespace

TextRom TextRom::parse(std::span<const u8> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("text rom is too small for its header");
    }
    const u32 tableOffset = readU32LE(file, 0);
    const u32 lumpCount = readU32LE(file, 4);
    if (tableOffset > file.size() ||
        usize{lumpCount} * kLumpHeaderSize > file.size() - tableOffset) {
        throw FormatError("text rom lump table lies outside the file");
    }

    std::vector<Lump> lumps;
    for (u32 i = 0; i < lumpCount; ++i) {
        const usize at = tableOffset + usize{i} * kLumpHeaderSize;
        Lump lump;
        lump.id = readU32LE(file, at);
        lump.offset = readU32LE(file, at + 4);
        lump.count = readU32LE(file, at + 8);
        if (lump.offset > tableOffset) {
            throw FormatError("text rom lump lies outside the file");
        }
        lumps.push_back(lump);
    }
    std::vector<u32> ends;
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

    const auto find = [&lumps](u32 id) -> const Lump* {
        const auto it = std::ranges::find_if(lumps, [id](const Lump& l) { return l.id == id; });
        return it == lumps.end() ? nullptr : &*it;
    };

    TextRom rom;
    if (const Lump* fonts = find(kFonts)) {
        for (u32 i = 0; i < fonts->count; ++i) {
            const usize at = fonts->offset + usize{i} * kFontEntrySize;
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
        const std::span<const u8> blob = file.subspan(text->offset, text->bytes);
        for (const u32 offset : readU32Table(file, *textOffsets, false)) {
            strings.push_back(offset < blob.size() ? readCString(blob, offset) : std::string{});
        }
    }

    std::vector<std::string> messageNames;
    std::vector<std::string> listNames;
    if (const Lump* names = find(kNames)) {
        const std::span<const u8> blob = file.subspan(names->offset, names->bytes);
        if (const Lump* offsets = find(kMessageNames)) {
            for (const u32 offset : readU32Table(file, *offsets, true)) {
                messageNames.push_back(readCString(blob, offset));
            }
        }
        if (const Lump* offsets = find(kListNames)) {
            for (const u32 offset : readU32Table(file, *offsets, true)) {
                listNames.push_back(readCString(blob, offset));
            }
        }
    }

    if (const Lump* messages = find(kMessages)) {
        for (u32 i = 0; i < messages->count; ++i) {
            const usize at = messages->offset + usize{i} * kMessageEntrySize;
            if (at + kMessageEntrySize > file.size()) {
                throw FormatError("text rom message table is truncated");
            }
            TextMessage message;
            const auto count = static_cast<s32>(readU32LE(file, at));
            const auto first = static_cast<s32>(readU32LE(file, at + 4));
            message.font = static_cast<s32>(readU32LE(file, at + 8));
            message.scale = std::bit_cast<f32>(readU32LE(file, at + 12));
            message.shadowScale = std::bit_cast<f32>(readU32LE(file, at + 16));
            for (s32 k = 0; k < count; ++k) {
                const usize index = static_cast<usize>(first) + static_cast<usize>(k);
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
        const std::vector<u32> indices = readU32Table(file, *listIndices, false);
        for (u32 i = 0; i < lists->count; ++i) {
            const usize at = lists->offset + usize{i} * kListEntrySize;
            if (at + kListEntrySize > file.size()) {
                throw FormatError("text rom list table is truncated");
            }
            TextMessageList list;
            const auto count = static_cast<s32>(readU32LE(file, at));
            const auto first = static_cast<s32>(readU32LE(file, at + 4));
            for (s32 k = 0; k < count; ++k) {
                const usize index = static_cast<usize>(first) + static_cast<usize>(k);
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

std::optional<usize> TextRom::findMessage(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (usize i = 0; i < messages.size(); ++i) {
        if (messages[i].name == key) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<usize> TextRom::findList(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (usize i = 0; i < lists.size(); ++i) {
        if (lists[i].name == key) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
