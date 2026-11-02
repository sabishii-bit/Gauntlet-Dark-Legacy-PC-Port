#include "expansions/n64/audio/LegendsAnnouncements.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"

#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/LegendsSpeech.h"

namespace gdl::game {
namespace {
struct Route {
    std::string_view name;
    u32 token;
};
// These are semantic equivalents, not presumed cross-platform numeric IDs.
// PPower speech is template+24; advice speech is e00722d0[row]+12, selected by
// e0071530. Tests compare those native records and their independent text IDs.
constexpr std::array kRoutes{
    // e0054fdc -> e004e668: native low health uses one complete phrase 4056.
    Route{"S_BADLY", 0x4056}, Route{"S_LIFEFORCE", 0x4056}, Route{"S_ABOUT", 0x4056},
    // e004d2b0 ordinary level gain; e004e6c0's It sign activation.
    Route{"S_GAINEDLEVEL", 0x4001}, Route{"S_NOWIT", 0x4006}, Route{"S_XSPEED", 0x4007},
    Route{"S_REFLECTVOX", 0x4008}, Route{"S_REFLECTSHVOX", 0x4009}, Route{"S_SUPERVOX", 0x400a},
    Route{"S_INVISVOX", 0x400b}, Route{"S_3WAYSHOTVOX", 0x400c}, Route{"S_5WAYSHOTVOX", 0x400d},
    Route{"S_INVULVOX", 0x400e}, Route{"S_FIREAMVOX", 0x4010}, Route{"S_LGHTNGAMVOX", 0x4011},
    Route{"S_ACIDAMVOX", 0x4012}, Route{"S_LIGHTAMVOX", 0x4013}, Route{"S_STOPPEDVOX", 0x4014},
    Route{"S_ANTIDEATHVOX", 0x4015}, Route{"S_FIREBRVOX", 0x4016}, Route{"S_LGHTNGBRVOX", 0x4017},
    Route{"S_ACIDBRVOX", 0x4018}, Route{"S_PHOENIXVOX", 0x4019}, Route{"S_GROWTHVOX", 0x401a},
    Route{"S_FIREWALLSHVOX", 0x401c}, Route{"S_LGHTNGSHVOX", 0x401d}, Route{"S_HAMMERVOX", 0x401e},
    Route{"S_RAPIDFIREVOX", 0x401f}, Route{"S_LEVVOX", 0x4020}, Route{"S_ICEAXEVOX", 0x4021},
    Route{"S_JAVELINVOX", 0x4022}, Route{"S_SCIMITARVOX", 0x4024}, Route{"S_SAVIORVOX", 0x4066},
    Route{"S_AVOID", 0x1090}, Route{"S_SAVEKEYS", 0x1008}, Route{"S_KEYFULL", 0x4025},
    Route{"S_MAGICFULL", 0x4026}, Route{"S_USEMAGIC2", 0x4027}, Route{"S_THROWMAGIC", 0x1084},
    Route{"S_SHIELDMAGIC", 0x1085}, Route{"S_DESTROY", 0x105b}, Route{"S_USEMAGIC", 0x4028},
    Route{"S_COLLECTGOLD", 0x108f}, Route{"S_COLLECTPOT", 0x1083}, Route{"S_TRANSPORTER", 0x108a},
    Route{"S_SAMEEXIT", 0x1006}, Route{"S_SHOOTINGMAGIC", 0x1086}, Route{"S_MEATGIVES", 0x108b},
    Route{"S_FRUITGIVES", 0x108c}, Route{"S_POISONEDFOOD", 0x108d}, Route{"S_MULTIPLEHITS", 0x1089},
    Route{"S_TRAPSMAKE", 0x1099}, Route{"S_USEKEY", 0x1088}, Route{"S_USEKEY2", 0x1009}};

std::optional<u32> tokenFor(std::span<const std::string_view> lines) {
    // The common help path sometimes prefixes "has". Native groups already
    // contain their own grammar. No other multi-line request may be split.
    if (lines.size() == 2 && lines.front() == "S_HAS")
        lines = lines.subspan(1);
    if (lines.size() != 1)
        return {};
    const auto found = std::ranges::find(kRoutes, lines.front(), &Route::name);
    return found == kRoutes.end() ? std::nullopt : std::optional{found->token};
}

struct Key {
    u32 token = 0;
    s32 character = -1;
    s32 color = -1;
    auto operator<=>(const Key&) const = default;
};

class PcmSource final : public StreamSource {
public:
    explicit PcmSource(SoundClip clip) : m_clip(std::move(clip)) {}
    AudioStreamDesc desc() const override { return {m_clip.sampleRate, m_clip.channels}; }
    bool read(std::vector<f32>& out, usize frames) override {
        const usize count = std::min(frames, m_clip.frames() - m_cursor);
        const auto samples =
            std::span{m_clip.samples}.subspan(m_cursor * m_clip.channels, count * m_clip.channels);
        out.insert(out.end(), samples.begin(), samples.end());
        m_cursor += count;
        return count != 0;
    }
    void rewind() override { m_cursor = 0; }

private:
    SoundClip m_clip;
    usize m_cursor = 0;
};

SoundClip decode(StreamSource& source) {
    const auto format = source.desc();
    if (format.sampleRate == 0 || format.sampleRate > 48000 || format.channels != 1)
        throw FormatError("unsupported native announcement PCM");
    SoundClip result{format.sampleRate, format.channels, {}};
    const usize limit = static_cast<usize>(format.sampleRate) * 30;
    while (source.read(result.samples, 4096)) {
        if (result.frames() > limit)
            throw FormatError("native announcement exceeds thirty seconds");
    }
    return result;
}
} // namespace

struct LegendsAnnouncements::Cache {
    std::map<Key, std::vector<u16>> routes;
    std::map<u16, SoundClip> clips;
    std::map<std::vector<u16>, f32> gains;
    f32 pcmGain = 1;
};

bool LegendsAnnouncements::load(const std::filesystem::path& root, const LegendsSpeech& speech) {
    m_cache.reset();
    if (!speech.opened())
        return false;
    try {
        FileStream file(root / "audio/mort.mdir");
        if (file.size() < 4 || file.size() > 64 * 1024 * 1024)
            throw FormatError("unsupported native announcement bank size");
        const auto bytes = file.readExact(static_cast<usize>(file.size()));
        Sha256 hash;
        hash.update(bytes);
        const auto identity = hash.hex();
        // One content-keyed cache is retained between levels. A replaced bank
        // cannot reuse old PCM; existing level owners keep their old cache alive.
        static std::mutex mutex;
        static std::string previousIdentity;
        static std::shared_ptr<const Cache> previous;
        const std::scoped_lock lock(mutex);
        if (previous && previousIdentity == identity) {
            m_cache = previous;
            return true;
        }
        MortBank bank;
        if (!bank.open(bytes))
            return false;
        auto cache = std::make_shared<Cache>();
        usize totalSamples = 0;
        const auto add = [&](u32 token, std::optional<AnnouncementSubject> subject) {
            const auto cue = subject ? speech.resolve(token, *subject) : speech.resolve(token);
            if (!cue)
                return;
            if (!cache->gains.contains(cue->clips)) {
                SoundClip phrase;
                for (const u16 id : cue->clips) {
                    if (!cache->clips.contains(id)) {
                        auto source = bank.createRestored(id);
                        if (!source)
                            return;
                        auto clip = decode(*source);
                        totalSamples += clip.samples.size();
                        if (totalSamples > 16 * 1024 * 1024)
                            throw FormatError("native announcement cache exceeds PCM budget");
                        cache->clips.emplace(id, std::move(clip));
                    }
                    const auto& part = cache->clips.at(id);
                    // A missing or silent word cannot authorize a partial
                    // native sentence in place of the complete GDL fallback.
                    if (!std::ranges::all_of(part.samples,
                                             [](f32 sample) { return std::isfinite(sample); }) ||
                        !std::ranges::any_of(part.samples, [](f32 sample) { return sample != 0; }))
                        return;
                    if (phrase.sampleRate != 0 && phrase.sampleRate != part.sampleRate)
                        throw FormatError("mixed-rate native announcement");
                    phrase.sampleRate = part.sampleRate;
                    phrase.channels = part.channels;
                    phrase.samples.insert(phrase.samples.end(), part.samples.begin(),
                                          part.samples.end());
                }
                // Calibrate the complete sentence, but retain each restored
                // word only once. This work occurs at asset load, never in ticks.
                PcmSource source(std::move(phrase));
                const auto calibration =
                    legends_audio_mix::calibrate(source, legends_audio_mix::Source::Speech);
                if (!calibration || !calibration->complete)
                    return;
                cache->pcmGain = std::max(cache->pcmGain, calibration->gain);
                cache->gains.emplace(cue->clips, calibration->gain);
            }
            const Key key{token, subject ? subject->character : -1, subject ? subject->color : -1};
            cache->routes.emplace(key, cue->clips);
        };
        for (const auto& route : kRoutes) {
            add(route.token, {});
            for (const s32 character : {0, 1, 2, 3, 8, 9, 10, 11})
                for (s32 color = 0; color < 4; ++color)
                    add(route.token, AnnouncementSubject{character, color});
        }
        // SoundPlayer clamps its stream volume at unity. Put the greatest
        // required gain in shared float PCM and use only attenuation per phrase.
        // The product is its one calibrated gain at every user slider setting;
        // float headroom reaches the existing final mixer limiter unchanged.
        for (auto& [id, clip] : cache->clips) {
            (void)id;
            for (f32& sample : clip.samples)
                sample *= cache->pcmGain;
        }
        m_cache = std::move(cache);
        previousIdentity = identity;
        previous = m_cache;
        return true;
    } catch (const std::exception& error) {
        log::warn("Native gameplay announcements: {}", error.what());
        return false;
    }
}

std::optional<SoundSequence>
LegendsAnnouncements::resolve(std::span<const std::string_view> lines,
                              std::optional<AnnouncementSubject> subject) const {
    if (!m_cache || (subject && subject->pojo))
        return {};
    const auto token = tokenFor(lines);
    if (!token)
        return {};
    const Key key{*token, subject ? subject->character : -1, subject ? subject->color : -1};
    const auto route = m_cache->routes.find(key);
    if (route == m_cache->routes.end())
        return {};
    SoundSequence result;
    for (const u16 id : route->second)
        result.steps.push_back({&m_cache->clips.at(id)});
    result.volume = m_cache->gains.at(route->second) / m_cache->pcmGain;
    result.bank = "N64/SUMNER";
    result.call = *token | (static_cast<u32>(key.character + 1) << 16U) |
                  (static_cast<u32>(key.color + 1) << 24U);
    return result;
}
} // namespace gdl::game
