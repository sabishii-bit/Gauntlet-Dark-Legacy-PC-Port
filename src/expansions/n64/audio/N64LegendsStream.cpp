#include "expansions/n64/audio/N64LegendsStream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

#include <amuse/AudioGroup.hpp>
#include <amuse/AudioGroupData.hpp>
#include <amuse/Engine.hpp>
#include <amuse/IBackendSubmix.hpp>
#include <amuse/IBackendVoice.hpp>
#include <amuse/Submix.hpp>
#include <amuse/Voice.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/audio/N64LegendsEffects.h"

namespace gdl {
namespace {
constexpr usize kFrames = 160;
constexpr f64 kInterval = 0.005;
constexpr f64 kRate = 32000.0;

struct NativeFile {
    std::string_view name;
    std::string_view sha256;
};
// These identifiers are fingerprints of user-supplied assets, not embedded game data.
// Amuse's sequence VM uses borrowed pointers without a span: do not pass unverified
// archives to it. New revisions require their own format and full-song corpus audit.
constexpr std::array<NativeFile, 4> kBank{{
    {"gauntlet.proj", "24a74774ea5d4cd6cb6e67ac00218b4fee4cb6ddb1eb93cf6c90f8e4e753dfe4"},
    {"gauntlet.pool", "cc047ca99b8eadaf413b2b4f393c31f5caa699fad4213e049b082fa2dc4ac6ce"},
    {"gauntlet.sdir", "fb85df064647d5ac82a8404415b2dd0ede2abad898fee56d8f9f8b4644518812"},
    {"gauntlet.samp", "08cd2cb3da046c676d78b5eee7d152400817d76691b117a70f3d21f7273cf833"},
}};
constexpr std::array<NativeFile, 39> kSongs{{
    {"mount1.song", "f6a7fbf61351b3eef4b64b43b68e7533e0187fe5142c156755aeeff5aead9e4f"},
    {"mount2.song", "cdb197514a461cf0e51fafdf2d1c41b5e6bff84c74022a6c427fc0a4b2e2320a"},
    {"mount3.song", "804a62112071217d0b2ab78c0c9306b527aed7c243b42ee7b0b6424b87dfffa6"},
    {"mountain4.song", "192653e7d2336eeebe1c887bf27b26816e2f8368c47687a3225dad3ab50a5185"},
    {"Mount5.song", "e9f564bd654cd0ec15e17a29117b403890df68fa277750b55229ab9e1eb2023d"},
    {"Mount6.song", "d888ce775a776497c552cc510cd90e2a0d9f9f0ae53b88ed2afb80b5b661e15c"},
    {"castle1.song", "10574a1a2d3bc0c883451aa5ab4734da480557a72ca09f0f82e02362713d3379"},
    {"Dungeon.song", "63c909cdf4d3368f5f46ca1f04494cbce7465a6d1674ead63db54ba7ea09f23f"},
    {"cASTLE3.song", "54723f3915d0f04d7c3c0a3f492415c396b40cfbb873a81f3a2459121dab1ff5"},
    {"castle4.song", "8ea0ec680753ad45d3c58c1c2424fd033cf5677452e665be21c96a3ab9c72b8b"},
    {"Castle5.song", "e61d36cecc8b634bc7606dd87edf08e86ad6840181d316cff9ac54abafaf70eb"},
    {"title.song", "e9694a29e6387ca15394b9effb5d2ac196b7511320154bbd58f9d8d14dfdfd7a"},
    {"Town1.song", "36959226271bee783d00ccf202069819970aa83b682ccae16c3fc5cffcaf097d"},
    {"Town2a.song", "b655a789026903a137a616ea96b62f0c728b7ce0b2e7c2b10caa8a65fec5802f"},
    {"town3.song", "89518fd7ce2eeb94c19e4245882a37153351acf5f58b4573bae224b4eee60e19"},
    {"town4.song", "4a49bbbcdf6326e97ad5f040e2a714eebe5a4e9978e51373fba6c991474ebb98"},
    {"LOOPING_TITS.song", "1ee478abdb13908edf3de44b0aac419b41137372f5f47780d9a189d8e0811be4"},
    {"Towerchoon.song", "951d78258a1c900e73d6acb667f94063357e519a331a9e54d90f6814c2014141"},
    {"Townt4.song", "4dc481e7a359349e003392ac0f2b586d71cb2e7860f756d51f8d99d11c603fff"},
    {"Ice1.song", "a0c29be1478dded48a8362f594f05b8e3959e14c720a6f34e8a2f00beb63edf8"},
    {"Cathedral2b.song", "16b4f51310a583f14e5e27d7130e71173f713eebcc1dc203f34d3f8967695aa8"},
    {"cathy2a.song", "ca8e2a7325e88a9d12b73222b632b0ea2e925c5708c2751a579fb31cc4d1c9ee"},
    {"battle1.song", "1c0c688355199015fd4ddfd2d7840e2726d5d4cda9f1ea768f3279be4e49fff5"},
    {"ChipmonkIntro.song", "a6a4adde62e10a0aaa952d1117d965132199bf017a8b47ddaaeaaa16d08c81cb"},
    {"Battle2.song", "96e620c80c180a25e86c8aa9016a879a3f451f7f3da57d9e37a49a86053742d9"},
    {"Battle3.song", "0b0c89c0eaf50d4291b1f1b46cc47b1bfb16081839eff7390c9c76480c91ef3c"},
    {"Ice3.song", "4aee995577023708eb7eb8c7d69d2837c26bdf9a7450c82ad73d0628a20d956f"},
    {"Deaf.song", "a11d9b9131abadc20a66624e6b069ccffb24e91a951e1d0f4809eab98539f844"},
    {"Cathywan.song", "54b621c9965b2179d6aea4c1e2699b93a8f7058a3ab5145d1b80bb05c0a9c9d4"},
    {"Ice4.song", "1f5dfd4699d98f808628f82a02046449328a18bd9c77e0d5ee9d45e8fa8563d1"},
    {"Extratune.song", "b51674f07d70a93ebc7d87454c3cb34ca09233769f296c6821c5327760b85842"},
    {"Hell1.song", "124de856ee708143153ee6620b9d0645710bec3af0d02799ffe175c255372083"},
    {"Hell2.song", "b1e675f708e763ba49e1c0aebb79aaed159f4af6299bb7d5c4236fbc5edbb4dd"},
    {"town5.song", "59042bac3400bb6dbca3bce013b04b57b829dd6559c627b823d1434575fd4eb6"},
    {"Booonus.song", "5febea139f33f71f8f1adc3351bb0c8cac3ed8c3a8fa8bd18b24e3a00c6dbd38"},
    {"Compmovie.song", "581b53bb33bd04ca5d7771ba00c523111eebdc673ec80e6389e4f14bbb1985d9"},
    {"Space.song", "b668aaf88ac81dcdcd1a51e747ef82f6fe484fd5fef11ca51e183fd31ad3db57"},
    {"Mushrooms.song", "f6842adf15bd2d192fa2faae1d6cf43a8c9a59ffdb73337069cbd12a93b2fd13"},
    {"Pirate.song", "4f10b31ef255ff81f3badec3b7018f57d2be7c6a99612bca421cf19ddc88f239"},
}};

std::vector<u8> readVerified(const std::filesystem::path& root, const NativeFile& file) {
    FileStream source(root / file.name);
    if (source.size() == 0 || source.size() > u64{4} * 1024 * 1024) {
        throw FormatError("unsupported N64 audio file length");
    }
    auto data = source.readExact(static_cast<usize>(source.size()));
    Sha256 digest;
    digest.update(data);
    if (digest.hex() != file.sha256) {
        throw FormatError("N64 audio requires the audited native US bank and sequences");
    }
    return data;
}

struct NativeBank {
    std::array<std::vector<u8>, 4> bytes;
    std::unique_ptr<amuse::AudioGroupData> group;

    explicit NativeBank(const std::filesystem::path& root) {
        for (usize i = 0; i < kBank.size(); ++i) {
            bytes[i] = readVerified(root, kBank[i]);
        }
        // The native relative PROJ accompanies 28-byte SDIR rows. Amuse's
        // corresponding reader expects 24: omit only the verified zero runtime
        // pointer at +8. Sample IDs, loops, pitch and encoded bytes are unchanged.
        auto nativeDirectory = std::move(bytes[2]);
        auto& directory = bytes[2];
        directory.reserve((nativeDirectory.size() - 4) / 28 * 24 + 4);
        for (usize at = 0; at + 28 <= nativeDirectory.size(); at += 28) {
            directory.insert(directory.end(),
                             nativeDirectory.begin() + static_cast<std::ptrdiff_t>(at),
                             nativeDirectory.begin() + static_cast<std::ptrdiff_t>(at + 8));
            directory.insert(directory.end(),
                             nativeDirectory.begin() + static_cast<std::ptrdiff_t>(at + 12),
                             nativeDirectory.begin() + static_cast<std::ptrdiff_t>(at + 28));
        }
        directory.insert(directory.end(), 4, 0xff);
        group = std::make_unique<amuse::AudioGroupData>(
            bytes[0].data(), bytes[0].size(), bytes[1].data(), bytes[1].size(), bytes[2].data(),
            bytes[2].size(), bytes[3].data(), bytes[3].size(), false, amuse::N64DataTag{});
    }
};

// Amuse's name databases are thread-local, but multiple sources may be pumped on
// one thread. Scope them around every library operation and restore the previous
// pointers so closing or rewinding one source cannot leave dangling global state.
struct NamesScope {
    std::array<amuse::NameDB**, 8> slots{
        &amuse::SongId::CurNameDB,       &amuse::SFXId::CurNameDB,    &amuse::GroupId::CurNameDB,
        &amuse::SoundMacroId::CurNameDB, &amuse::SampleId::CurNameDB, &amuse::TableId::CurNameDB,
        &amuse::KeymapId::CurNameDB,     &amuse::LayersId::CurNameDB};
    std::array<amuse::NameDB*, 8> saved{};
    NamesScope(const NamesScope&) = delete;
    NamesScope& operator=(const NamesScope&) = delete;
    explicit NamesScope(const amuse::ProjectDatabase& names) {
        for (usize i = 0; i < slots.size(); ++i) {
            saved[i] = *slots[i];
        }
        names.setIdDatabases();
    }
    ~NamesScope() {
        for (usize i = 0; i < slots.size(); ++i) {
            *slots[i] = saved[i];
        }
    }
};

struct Backend;

struct Mix final : amuse::IBackendSubmix {
    Backend& parent;
    amuse::Submix& client;
    bool mainOut;
    s32 bus;
    Mix* send = nullptr;
    f32 sendLevel = 0.0f;
    std::array<f32, kFrames * 2> data{};

    Mix(Backend& owner, amuse::Submix& source, bool main, s32 identity)
        : parent(owner), client(source), mainOut(main), bus(identity) {}
    ~Mix() override;
    Mix(const Mix&) = delete;
    Mix& operator=(const Mix&) = delete;
    void setSendLevel(amuse::IBackendSubmix* target, float value, bool /*slew*/) override {
        send = dynamic_cast<Mix*>(target);
        sendLevel = value;
    }
    double getSampleRate() const override { return kRate; }
    amuse::SubmixFormat getSampleFormat() const override { return amuse::SubmixFormat::Float; }
};

struct Voice final : amuse::IBackendVoice {
    Backend& parent;
    amuse::Voice& client;
    const u64 identity;
    f64 rate;
    f64 ratio = 1.0;
    f64 phase = 0.0;
    bool playing = false;
    std::vector<s16> cache;
    std::vector<std::pair<Mix*, std::array<f32, 8>>> routes;

    Voice(Backend& owner, amuse::Voice& source, f64 frequency, u64 serial)
        : parent(owner), client(source), identity(serial), rate(frequency) {}
    ~Voice() override;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    void resetSampleRate(double value) override {
        rate = value;
        cache.clear();
        phase = 0.0;
    }
    void resetChannelLevels() override { routes.clear(); }
    void setChannelLevels(amuse::IBackendSubmix* mix, const std::array<float, 8>& levels,
                          bool /*slew*/) override {
        for (auto& route : routes) {
            if (route.first == mix) {
                route.second = levels;
                return;
            }
        }
        if (routes.size() >= 3) {
            throw FormatError("N64 music exceeded its three native audio buses");
        }
        routes.emplace_back(dynamic_cast<Mix*>(mix), levels);
    }
    void setPitchRatio(double value, bool /*slew*/) override { ratio = value; }
    void start() override { playing = true; }
    void stop() override { playing = false; }
    void pump();
};

struct Backend final : amuse::IBackendVoiceAllocator {
    std::vector<Voice*> voices;
    std::vector<Mix*> mixes;
    amuse::Engine* engine = nullptr;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    u64 nextVoice = 1;

    std::unique_ptr<amuse::IBackendVoice> allocateVoice(amuse::Voice& source, double rate,
                                                        bool /*dynamicPitch*/) override {
        if (voices.size() >= 256) {
            throw FormatError("N64 music exceeded its voice budget");
        }
        auto result = std::make_unique<Voice>(*this, source, rate, nextVoice++);
        voices.push_back(result.get());
        return result;
    }
    std::unique_ptr<amuse::IBackendSubmix> allocateSubmix(amuse::Submix& source, bool main,
                                                          int bus) override {
        if (mixes.size() >= 3 || bus < 0 || bus > 2) {
            throw FormatError("N64 music exceeded its mixer budget");
        }
        auto result = std::make_unique<Mix>(*this, source, main, bus);
        mixes.push_back(result.get());
        return result;
    }
    std::vector<std::pair<std::string, std::string>> enumerateMIDIDevices() override { return {}; }
    std::unique_ptr<amuse::IMIDIReader> allocateMIDIReader(amuse::Engine& /*engine*/) override {
        return {};
    }
    amuse::AudioChannelSet getAvailableSet() override { return amuse::AudioChannelSet::Stereo; }
    void setVolume(float value) override { volume = value; }
    void setCallbackInterface(amuse::Engine* value) override { engine = value; }

    std::array<f32, kFrames * 2> pump() {
        for (auto* mix : mixes) {
            mix->data.fill(0.0f);
        }
        engine->_on5MsInterval(*this, kInterval);
        // Macros can create voices or immediately kill an earlier keygroup.
        // Allocation identities remain ordered after either operation, unlike
        // vector indexes or addresses that an allocator can reuse in this block.
        u64 processed = 0;
        usize operations = 0;
        while (true) {
            const auto next =
                std::find_if(voices.begin(), voices.end(), [processed](const Voice* voice) {
                    return voice->identity > processed;
                });
            if (next == voices.end()) {
                break;
            }
            if (++operations > 1024) {
                throw FormatError("N64 music exceeded its per-block voice work budget");
            }
            Voice* voice = *next;
            processed = voice->identity;
            if (!voice->playing) {
                continue;
            }
            const auto client = engine->findVoice(voice->client.vid());
            if (!client) {
                throw FormatError("N64 music lost an active voice");
            }
            client->preSupplyAudio(kInterval);
            // A child macro can kill its parent keygroup, resetting this
            // backend too. Retain the library token across the callback and
            // reacquire the backend only if its allocation is still alive.
            const auto live =
                std::find_if(voices.begin(), voices.end(), [processed](const Voice* candidate) {
                    return candidate->identity == processed;
                });
            if (live != voices.end()) {
                (*live)->pump();
            }
        }
        const amuse::ChannelMap channels{
            2, {amuse::AudioChannel::FrontLeft, amuse::AudioChannel::FrontRight}};
        for (auto* mix : mixes) {
            if (!mix->mainOut) {
                mix->client.applyEffect(mix->data.data(), kFrames, channels);
                if (mix->send != nullptr) {
                    for (usize i = 0; i < mix->data.size(); ++i) {
                        mix->send->data[i] += mix->data[i] * mix->sendLevel;
                    }
                }
            }
        }
        std::array<f32, kFrames * 2> result{};
        for (auto* mix : mixes) {
            if (mix->mainOut) {
                mix->client.applyEffect(mix->data.data(), kFrames, channels);
                for (usize i = 0; i < result.size(); ++i) {
                    result[i] += mix->data[i] * volume;
                }
            }
        }
        for (f32& sample : result) {
            if (!std::isfinite(sample)) {
                throw FormatError("N64 music produced non-finite audio");
            }
        }
        engine->_onPumpCycleComplete(*this);
        return result;
    }
};

Mix::~Mix() {
    std::erase(parent.mixes, this);
}
Voice::~Voice() {
    std::erase(parent.voices, this);
}

void Voice::pump() {
    if (!playing) {
        return;
    }
    const f64 step = rate * ratio * parent.pitch / kRate;
    if (!std::isfinite(step) || step <= 0.0 || step > 100.0) {
        throw FormatError("N64 music has an invalid voice frequency");
    }
    const usize needed = static_cast<usize>(std::floor(phase + kFrames * step)) + 2;
    if (cache.size() < needed) {
        const usize begin = cache.size();
        cache.resize(needed);
        const usize supplied =
            client.supplyAudio(needed - begin, std::span{cache}.subspan(begin).data());
        if (supplied > needed - begin) {
            throw FormatError("N64 music supplied too many voice samples");
        }
        std::fill(cache.begin() + static_cast<std::ptrdiff_t>(begin + supplied), cache.end(), 0);
    }
    std::array<f32, kFrames> input{};
    std::array<f32, kFrames> routed{};
    for (usize i = 0; i < kFrames; ++i) {
        const auto index = static_cast<usize>(phase);
        const auto fraction = static_cast<f32>(phase - static_cast<f64>(index));
        input[i] = (static_cast<f32>(cache[index]) * (1.0f - fraction) +
                    static_cast<f32>(cache[index + 1]) * fraction) /
                   32768.0f;
        phase += step;
    }
    const auto consumed = static_cast<usize>(phase);
    phase -= static_cast<f64>(consumed);
    cache.erase(cache.begin(), cache.begin() + static_cast<std::ptrdiff_t>(consumed));
    for (const auto& [mix, gains] : routes) {
        client.routeAudio(kFrames, kInterval, mix->bus, input.data(), routed.data());
        for (usize i = 0; i < kFrames; ++i) {
            mix->data[i * 2] += routed[i] * gains[0];
            mix->data[i * 2 + 1] += routed[i] * gains[1];
        }
    }
}

class NativeEffect final : public StreamSource {
public:
    NativeEffect(std::shared_ptr<const NativeBank> bank, u16 cue, f32 pitch, f32 volume, f32 pan)
        : m_bank(std::move(bank)), m_cue(cue), m_volume(volume), m_pan(pan) {
        m_backend.pitch = pitch;
        start();
    }
    ~NativeEffect() override {
        const NamesScope scope(m_names);
        m_engine.reset();
    }
    NativeEffect(const NativeEffect&) = delete;
    NativeEffect& operator=(const NativeEffect&) = delete;
    AudioStreamDesc desc() const override { return {32000, 2}; }
    bool read(std::vector<f32>& out, usize frames) override {
        if (!m_engine || frames == 0) {
            return false;
        }
        const usize begin = out.size();
        try {
            const NamesScope scope(m_names);
            const usize count = std::min<usize>(frames, 32768);
            out.reserve(out.size() + count * 2);
            for (usize i = 0; i < count; ++i) {
                if (m_pendingFrame == kFrames) {
                    if (m_finished) {
                        break;
                    }
                    m_pending = m_backend.pump();
                    m_pendingFrame = 0;
                    if (m_engine->getNumTotalActiveVoices() == 0) {
                        // Let the native studio's reverb/chorus finish after
                        // the last macro ends. A silent 100 ms tail is done;
                        // bound a residual denormal/effect tail to five seconds.
                        m_tailFrames += kFrames;
                        const bool silent = std::ranges::all_of(
                            m_pending, [](f32 value) { return std::abs(value) < .000001f; });
                        m_silentFrames = silent ? m_silentFrames + kFrames : 0;
                        m_finished = m_silentFrames >= 3200 || m_tailFrames >= 160000;
                    }
                }
                const usize at = m_pendingFrame++ * 2;
                out.push_back(m_pending[at]);
                out.push_back(m_pending[at + 1]);
            }
        } catch (const std::exception& error) {
            log::warn("N64 effect {} stopped: {}", m_cue, error.what());
            const NamesScope scope(m_names);
            m_engine.reset();
        }
        return out.size() != begin;
    }
    void rewind() override {
        try {
            start();
        } catch (const std::exception& error) {
            log::warn("N64 effect {} rewind failed: {}", m_cue, error.what());
            const NamesScope scope(m_names);
            m_engine.reset();
        }
    }

private:
    void start() {
        const NamesScope scope(m_names);
        m_engine.reset();
        m_engine = std::make_unique<amuse::Engine>(m_backend);
        m_engine->addAudioGroup(*m_bank->group);
        if (!m_engine->fxStart(amuse::SFXId(m_cue), m_volume, m_pan)) {
            throw FormatError("N64 effect sound macro could not start");
        }
        m_pendingFrame = kFrames;
        m_tailFrames = 0;
        m_silentFrames = 0;
        m_finished = false;
    }
    std::shared_ptr<const NativeBank> m_bank;
    u16 m_cue;
    f32 m_volume;
    f32 m_pan;
    amuse::ProjectDatabase m_names;
    Backend m_backend;
    std::unique_ptr<amuse::Engine> m_engine;
    std::array<f32, kFrames * 2> m_pending{};
    usize m_pendingFrame = kFrames;
    usize m_tailFrames = 0;
    usize m_silentFrames = 0;
    bool m_finished = false;
};
} // namespace

struct N64LegendsStream::Impl {
    amuse::ProjectDatabase names;
    std::shared_ptr<const NativeBank> bank;
    std::vector<u8> song;
    Backend backend;
    std::unique_ptr<amuse::Engine> engine;
    amuse::ObjToken<amuse::Sequencer> sequence;
    u16 songId = 0;
    std::array<f32, kFrames * 2> pending{};
    usize pendingFrame = kFrames;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        const NamesScope scope(names);
        sequence.reset();
        engine.reset();
    }

    void start() {
        const NamesScope scope(names);
        sequence.reset();
        engine.reset();
        engine = std::make_unique<amuse::Engine>(backend);
        // The sequencer master runs through each instrument's nonlinear volume
        // curve. Keep it neutral; the common N64 playback adapter calibrates
        // decoded PCM before the category slider and final mixer limiter.
        engine->setVolume(1.0f);
        engine->addAudioGroup(*bank->group);
        sequence =
            engine->seqPlay(amuse::GroupId(songId), amuse::SongId(songId), song.data(), true);
        if (!sequence) {
            throw FormatError("N64 music sequencer could not start the requested song");
        }
        pendingFrame = kFrames;
    }
};

N64LegendsStream::N64LegendsStream() = default;
N64LegendsStream::~N64LegendsStream() = default;

bool N64LegendsStream::open(const std::filesystem::path& root, u16 songId) {
    m_impl.reset();
    try {
        if (songId >= kSongs.size()) {
            throw FormatError("invalid native N64 song ID");
        }
        auto result = std::make_unique<Impl>();
        result->songId = songId;
        result->bank = std::make_shared<NativeBank>(root);
        result->song = readVerified(root, kSongs[songId]);
        result->start();
        m_impl = std::move(result);
        return true;
    } catch (const std::exception& error) {
        log::warn("N64 music {}: {}", paths::utf8(root), error.what());
        return false;
    }
}

bool N64LegendsStream::opened() const {
    return m_impl != nullptr;
}

AudioStreamDesc N64LegendsStream::desc() const {
    return opened() ? AudioStreamDesc{32000, 2} : AudioStreamDesc{0, 0};
}

bool N64LegendsStream::read(std::vector<f32>& out, usize frames) {
    if (!m_impl || frames == 0) {
        return false;
    }
    try {
        const NamesScope scope(m_impl->names);
        const usize count = std::min<usize>(frames, 32768);
        out.reserve(out.size() + count * 2);
        for (usize i = 0; i < count; ++i) {
            if (m_impl->pendingFrame == kFrames) {
                // Preserve float headroom until AudioMixer limits the combined
                // output, after the user's music/master settings and resampling.
                m_impl->pending = m_impl->backend.pump();
                m_impl->pendingFrame = 0;
            }
            const usize at = m_impl->pendingFrame++ * 2;
            out.push_back(m_impl->pending[at]);
            out.push_back(m_impl->pending[at + 1]);
        }
        return true;
    } catch (const std::exception& error) {
        log::warn("N64 music stopped: {}", error.what());
        m_impl.reset();
        return false;
    }
}

void N64LegendsStream::rewind() {
    if (m_impl) {
        try {
            m_impl->start();
        } catch (const std::exception& error) {
            log::warn("N64 music rewind failed: {}", error.what());
            m_impl.reset();
        }
    }
}

// Effects and songs deliberately share this translation unit's bounded native
// bank reader and software audio backend. They never share live voice state.
struct N64LegendsEffects::Impl {
    std::shared_ptr<const NativeBank> bank;
    std::vector<u16> cues;
    std::vector<std::weak_ptr<StreamSource>> sources;
};

N64LegendsEffects::N64LegendsEffects() = default;
N64LegendsEffects::~N64LegendsEffects() = default;

bool N64LegendsEffects::open(const std::filesystem::path& root) {
    m_impl.reset();
    try {
        auto result = std::make_unique<Impl>();
        result->bank = std::make_shared<NativeBank>(root);
        const amuse::ProjectDatabase names{};
        const NamesScope scope(names);
        const auto project =
            amuse::AudioGroupProject::CreateAudioGroupProject(*result->bank->group);
        // Native startup e000c188 pushes SFX group 39. Song groups are not SFX.
        const auto* effects = project.getSFXGroupIndex(amuse::GroupId(39));
        if (!effects) {
            throw FormatError("N64 effects group39 is missing");
        }
        for (const auto& [cue, entry] : effects->m_sfxEntries) {
            static_cast<void>(entry);
            result->cues.push_back(cue.id);
        }
        std::ranges::sort(result->cues);
        m_impl = std::move(result);
        return true;
    } catch (const std::exception& error) {
        log::warn("N64 effects {}: {}", paths::utf8(root), error.what());
        return false;
    }
}

bool N64LegendsEffects::opened() const {
    return m_impl != nullptr;
}

bool N64LegendsEffects::contains(u16 cue) const {
    return m_impl && std::ranges::binary_search(m_impl->cues, cue);
}

std::span<const u16> N64LegendsEffects::cueIds() const {
    return m_impl ? std::span<const u16>{m_impl->cues} : std::span<const u16>{};
}

std::shared_ptr<StreamSource> N64LegendsEffects::create(u16 cue, f32 pitch, f32 volume, f32 pan) {
    if (!contains(cue) || !std::isfinite(pitch) || pitch <= 0 || pitch > 8 ||
        !std::isfinite(volume) || volume < 0 || volume > 1 || !std::isfinite(pan) || pan < -1 ||
        pan > 1) {
        return nullptr;
    }
    std::erase_if(m_impl->sources, [](const auto& source) { return source.expired(); });
    if (m_impl->sources.size() >= 64) {
        return nullptr;
    }
    try {
        auto source = std::make_shared<NativeEffect>(m_impl->bank, cue, pitch, volume, pan);
        m_impl->sources.emplace_back(source);
        return source;
    } catch (const std::exception& error) {
        log::warn("N64 effect {} could not start: {}", cue, error.what());
        return nullptr;
    }
}
} // namespace gdl
