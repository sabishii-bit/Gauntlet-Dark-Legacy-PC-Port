#include "game/screens/PartyNames.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/screens/PartyFigures.h"

namespace gdl::game {

namespace {

constexpr std::string_view kFontFile = "fonts/initials.json";
constexpr std::string_view kFontTexture = "INITIALS";
constexpr f32 kBehind = 1e-4f; ///< a clip w under this is at or behind the eye

} // namespace

bool PartyNames::load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                      TextureSet& staticTextures) {
    clear();
    const auto sheet = staticTextures.loaded() ? staticTextures.find(kFontTexture) : std::nullopt;
    if (!sheet.has_value() || !m_font.load(unpackedRoot / kFontFile, kSpaceWidth)) {
        log::warn("Play: the initials font is not unpacked; names are not shown");
        return false;
    }
    try {
        m_text.setFont(&m_font, &staticTextures.texture(device, *sheet));
    } catch (const std::exception& e) {
        log::warn("Play: initials font: {}", e.what());
        return false;
    }
    return true;
}

void PartyNames::clear() {
    m_text.setFont(nullptr, nullptr);
    m_held = false;
}

std::string PartyNames::shownName(std::string_view saveName) {
    std::string shown{saveName.substr(0, std::min(saveName.size(), kLetters))};
    std::ranges::replace(shown, '_', ' ');
    return shown;
}

std::optional<Vec2> PartyNames::screenOf(const Mat4& clip, const Vec3& point,
                                         const Mat4& canvasProjection) {
    const Vec4 at = clip * Vec4{point, 1.0f};
    if (at.w <= kBehind) {
        return std::nullopt;
    }
    const Vec4 onCanvas = glm::inverse(canvasProjection) * at;
    return Vec2{onCanvas} / onCanvas.w;
}

void PartyNames::show(std::span<PlayerRuntime> players) {
    for (PlayerRuntime& runtime : players) {
        runtime.nameTicks = kTicks;
    }
}

void PartyNames::step(std::span<PlayerRuntime> players, s32 ticks, bool held) {
    m_held = held;
    if (held) {
        return;
    }
    for (PlayerRuntime& runtime : players) {
        runtime.nameTicks = std::max(runtime.nameTicks - ticks, 0);
    }
}

void PartyNames::draw(Canvas& canvas, std::span<const PlayerRuntime> players, const Mat4& clip,
                      const Mat4& canvasProjection, f32 frameBlend) const {
    if (m_held || !m_text.ready()) {
        return;
    }
    TextStyle style;
    style.scale = kScale;
    style.color = Color::white();
    for (const PlayerRuntime& runtime : players) {
        if (runtime.nameTicks <= 0 || runtime.life != PlayerLife::Standing) {
            continue;
        }
        const Vec3 offset = runtime.actor.attentionPoint() - runtime.actor.position();
        const Vec3 anchor = Vec3{PartyFigures::presentationBody(runtime, frameBlend)[3]} + offset;
        const std::optional<Vec2> at = screenOf(clip, anchor, canvasProjection);
        if (!at.has_value()) {
            continue;
        }
        // A negative x is centred on it, as DrawTextKeepScale's is.
        m_text.draw(canvas, -static_cast<s32>(at->x), static_cast<s32>(at->y),
                    shownName(runtime.actor.save().name), style);
    }
}

} // namespace gdl::game
