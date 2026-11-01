#include "expansions/n64/audio/LegendsSpeech.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/formats/LegendsN64Text.h"

namespace gdl::game {
namespace {
constexpr u32 kProgramBase = 0xe0000400U;
constexpr usize kMaximumPending = 32;

usize offset(std::span<const u8> program, u32 address, usize size) {
    if (address < kProgramBase || address - kProgramBase > program.size() ||
        size > program.size() - (address - kProgramBase)) {
        throw FormatError("truncated native N64 speech metadata");
    }
    return address - kProgramBase;
}

std::string component(std::span<const u8> program, u32 address) {
    std::string result;
    for (u32 i = 0; i <= 32; ++i) {
        if (address > 0xffffffffU - i) {
            break;
        }
        const u8 value = program[offset(program, address + i, 1)];
        if (value == 0 && !result.empty()) {
            return result;
        }
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_')) {
            break;
        }
        result.push_back(static_cast<char>(value));
    }
    throw FormatError("invalid native N64 speech map name");
}
} // namespace

bool LegendsSpeech::openN64(const std::filesystem::path& root) {
    m_entries.clear();
    m_groups.clear();
    m_colors = {};
    m_classes = {};
    m_levelCues.clear();
    m_obeliskCues = {};
    m_deathAdvice.reset();
    m_pending.clear();
    m_bank = {};
    try {
        FileStream file(root / "_program/game.bin");
        if (file.size() < 0x80000 || file.size() > u64{4} * 1024 * 1024) {
            throw FormatError("unsupported native N64 speech program");
        }
        const auto program = file.readExact(static_cast<usize>(file.size()));
        Sha256 hash;
        hash.update(program);
        if (hash.hex() != "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585") {
            throw FormatError("N64 speech routing requires the audited US program");
        }
        // e007a0c0 resolves a token or indexed alias. Its fallback is a separate
        // record at e007b4fc, not the last of the 124 indexed records.
        const auto entryAt = [&](u32 address) {
            const usize at = offset(program, address, 20);
            Entry entry{readU16BE(program, at), readU16BE(program, at + 2), program[at + 7],
                        std::bit_cast<f32>(readU32BE(program, at + 12))};
            if (!std::isfinite(entry.lifetime) || entry.lifetime < 0 || entry.lifetime > 3600) {
                throw FormatError("invalid native N64 speech queue lifetime");
            }
            return entry;
        };
        if (readU32BE(program, offset(program, 0xe007b510U, 4)) != 124) {
            throw FormatError("unsupported native N64 speech routing count");
        }
        std::vector<Entry> entries;
        for (u32 i = 0; i < 124; ++i) {
            entries.push_back(entryAt(0xe007ab4cU + i * 20));
        }
        const Entry defaultEntry = entryAt(0xe007b4fcU);
        std::vector<std::vector<u16>> groups;
        for (u32 i = 0; i < 110; ++i) {
            const u32 pointer = readU32BE(program, offset(program, 0xe007cc2cU + i * 4, 4));
            const usize at = offset(program, pointer, 2);
            const usize count = readU16BE(program, at);
            if (count > 6) {
                throw FormatError("native N64 speech phrase exceeds six clips");
            }
            offset(program, pointer, 2 + count * 2);
            std::vector<u16> clips;
            for (usize j = 0; j < count; ++j) {
                clips.push_back(readU16BE(program, at + 2 + j * 2));
            }
            groups.push_back(std::move(clips));
        }

        std::map<u32, std::string> realms;
        bool terminated = false;
        for (u32 i = 0; i < 11; ++i) {
            const usize at = offset(program, 0xe006c360U + i * 24, 24);
            const u32 id = readU32BE(program, at);
            if (id == 16) {
                terminated = true;
                break;
            }
            if (id >= 16 ||
                !realms.emplace(id, component(program, readU32BE(program, at + 8))).second) {
                throw FormatError("invalid native N64 speech realm table");
            }
        }
        if (!terminated) {
            throw FormatError("unterminated native N64 speech realm table");
        }
        std::map<std::string, u32, std::less<>> levelCues;
        terminated = false;
        for (u32 i = 0; i < 128; ++i) {
            const usize at = offset(program, 0xe006c468U + i * 40, 40);
            const u32 realm = readU32BE(program, at);
            if (realm == 16) {
                terminated = true;
                break;
            }
            const auto stem = realms.find(realm);
            if (stem == realms.end() || readU32BE(program, at + 4) > 10) {
                throw FormatError("invalid native N64 speech level table");
            }
            const auto name =
                toLowerAscii(stem->second + component(program, readU32BE(program, at + 12)));
            // e006ab40 stores level+36; e0037824 stages it for e0038a90,
            // which plays once when the entry camera has a valid player target.
            if (!levelCues.emplace(name, readU16BE(program, at + 36)).second) {
                throw FormatError("duplicate native N64 speech level name");
            }
        }
        if (!terminated) {
            throw FormatError("unterminated native N64 speech level table");
        }
        std::array<u32, 3> obeliskCues{};
        constexpr std::array<u32, 3> kAdvice{29, 32, 34};
        for (usize i = 0; i < kAdvice.size(); ++i) {
            // e0068000 chooses these advice rows for obelisks1..3,4..5,6..7.
            // e0071530 plays row+12 through e007a13c (priority5).
            obeliskCues[i] =
                readU32BE(program, offset(program, 0xe00722d0U + kAdvice[i] * 24 + 12, 4));
        }
        MortBank bank;
        // e0065560 calls advice8 after replacing a statue with live Death.
        // e0071530 uses row+0 as the lowest-party-level ceiling, row+8 as
        // the caption ID and row+12 as a separate concrete speech phrase.
        const usize advice = offset(program, 0xe00722d0U + 8 * 24, 24);
        if (readU32BE(program, advice) != 10 || readU32BE(program, advice + 4) != 0 ||
            readU32BE(program, advice + 8) != 18 || readU32BE(program, advice + 12) != 0x4028) {
            throw FormatError("unsupported native N64 Death advice");
        }
        const AssetLocator files{root};
        if (const auto library = files.find("strlib.wad")) {
            if (const auto reference = formats::legendsN64StringReference(
                    readFile(*library), readU32BE(program, advice + 8))) {
                if (const auto textPath = files.find("english/" + reference->bank + ".wad")) {
                    const auto pages = formats::decodeLegendsN64Text(readFile(*textPath));
                    if (reference->index < pages.size() && !pages[reference->index].empty()) {
                        m_deathAdvice = LegendsDeathAdvice{
                            static_cast<s32>(readU32BE(program, advice)),
                            readU32BE(program, advice + 12), pages[reference->index]};
                    }
                }
            }
        }
        // The caption remains available if the optional narration bank is absent.
        if (!bank.open(root / "audio/mort.mdir")) {
            return false;
        }
        m_bank = std::move(bank);
        m_entries = std::move(entries);
        m_default = defaultEntry;
        m_groups = std::move(groups);
        // e007c730 uses these independent tables for -1 color and -2 class.
        for (usize i = 0; i < m_colors.size(); ++i)
            m_colors[i] = readU16BE(program, offset(program, 0xe007c850U + u32(i) * 2, 2));
        for (usize i = 0; i < m_classes.size(); ++i)
            m_classes[i] = readU16BE(program, offset(program, 0xe007c858U + u32(i) * 2, 2));
        m_levelCues = std::move(levelCues);
        m_obeliskCues = obeliskCues;
        return true;
    } catch (const std::exception& error) {
        log::warn("Legends speech {}: {}", paths::utf8(root), error.what());
        return false;
    }
}

std::optional<LegendsSpeechCue> LegendsSpeech::resolve(u32 token) const {
    return resolve(token, nullptr);
}

std::optional<LegendsSpeechCue> LegendsSpeech::resolve(u32 token,
                                                       const AnnouncementSubject& subject) const {
    return resolve(token, &subject);
}

std::optional<LegendsSpeechCue> LegendsSpeech::resolve(u32 token,
                                                       const AnnouncementSubject* subject) const {
    s32 nativeClass = -1;
    if (subject) {
        // e004eefc/e004dc64: four originals, then their secret counterparts
        // at native slots5..8. Slot4 is invalid; native Sumner slot9 says Wizard,
        // so it is not an equivalent spoken identity for the GDL Sumner class.
        if (subject->pojo || subject->color < 0 || subject->color >= 4)
            return std::nullopt;
        if (subject->character >= 0 && subject->character < 4)
            nativeClass = subject->character;
        else if (subject->character >= 8 && subject->character < 12)
            nativeClass = subject->character - 3;
        else
            return std::nullopt;
    }
    if (!opened() || token == 0xffff || token == 0xffffffff) {
        return std::nullopt;
    }
    Entry entry = m_default;
    if ((token & 0x10000U) != 0) {
        if ((token & ~0x10fffU) != 0 || (token & 0xfffU) >= m_entries.size()) {
            return std::nullopt;
        }
        entry = m_entries[token & 0xfffU];
    } else {
        if (token > 0xffff) {
            return std::nullopt;
        }
        const auto found = std::ranges::find(m_entries, token, &Entry::token);
        if (found != m_entries.end()) {
            entry = *found;
        }
    }
    if (entry.replacement != 0xffff) {
        token = entry.replacement;
    }
    LegendsSpeechCue cue;
    cue.lifetimeSeconds = entry.lifetime;
    cue.clearQueue = (entry.flags & 8U) != 0;
    if ((token & 0x1000U) != 0) {
        cue.clips.push_back(static_cast<u16>(token & 0x3ffU));
    } else if ((token & 0x4000U) != 0) {
        const usize group = token & 0x8fffU;
        if (group >= m_groups.size()) {
            return std::nullopt;
        }
        for (u16 clip : m_groups[group]) {
            // Resolve the whole phrase before admission. Never turn an absent
            // player or unsupported rank into clip1023/1022/1021.
            if (clip == 0xffffU && subject)
                clip = m_colors[static_cast<usize>(subject->color)];
            else if (clip == 0xfffeU && subject)
                clip = m_classes[static_cast<usize>(nativeClass)];
            if (clip == 0 || clip >= 0xfffdU) {
                return std::nullopt;
            }
            cue.clips.push_back(static_cast<u16>(clip & 0x3ffU));
        }
    }
    if (cue.clips.empty() ||
        !std::ranges::all_of(cue.clips, [&](u16 id) { return m_bank.contains(id); })) {
        return std::nullopt;
    }
    return cue;
}

std::optional<u32> LegendsSpeech::entryCue(std::string_view mapName) const {
    const auto found = m_levelCues.find(toLowerAscii(mapName));
    if (found == m_levelCues.end() || !resolve(found->second)) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<u32> LegendsSpeech::obeliskCue(u32 identity) const {
    if (identity < 1 || identity > 7) {
        return std::nullopt;
    }
    const u32 token = m_obeliskCues[identity <= 3 ? 0 : identity <= 5 ? 1 : 2];
    return resolve(token) ? std::optional<u32>(token) : std::nullopt;
}

bool LegendsSpeech::enqueue(SoundPlayer& output, u32 token, s32 priority) {
    const auto cue = resolve(token);
    if (!cue || priority < 0 || priority > 255) {
        return false;
    }
    if (cue->clearQueue) {
        stop(output);
    }
    if (m_pending.size() >= kMaximumPending) {
        return false;
    }
    // e000e660 orders higher priorities first, retaining FIFO for equal values.
    const auto next = std::ranges::find_if(
        m_pending, [&](const Pending& item) { return item.priority < priority; });
    m_pending.insert(next, Pending{*cue, priority, 0});
    update(output, 0, m_paused);
    return true;
}

void LegendsSpeech::update(SoundPlayer& output, f32 seconds, bool paused) {
    m_paused = paused;
    if (paused || !std::isfinite(seconds) || seconds < 0) {
        return;
    }
    for (auto& pending : m_pending) {
        pending.age += seconds;
    }
    // Native e000c458/e000f370 expire waiting phrases, never a clip that has
    // already started. A phrase's concrete clips stay in one finite stream.
    std::erase_if(m_pending,
                  [](const Pending& item) { return item.age > item.cue.lifetimeSeconds; });
    if (output.isPlaying(m_handle)) {
        return;
    }
    m_handle = kNoSound;
    while (!m_pending.empty()) {
        auto cue = std::move(m_pending.front().cue);
        m_pending.erase(m_pending.begin());
        auto source = m_bank.createRestored(cue.clips);
        if (source) {
            m_handle = output.playStream(
                legendsAudioMix::prepare(std::move(source), legendsAudioMix::Source::Speech), false,
                1, SoundCategory::Effects);
            if (m_handle != kNoSound) {
                return;
            }
        }
    }
}

void LegendsSpeech::stop(SoundPlayer& output) {
    output.stop(m_handle);
    m_handle = kNoSound;
    m_pending.clear();
}

} // namespace gdl::game
