#include "game/players/TurboMeter.h"

#include <algorithm>
#include <cstdint>

namespace gdl::game {

void TurboMeter::reset() {
    m_held = 0.0f;
    m_shown = 0.0f;
    m_flash = Flash::None;
    m_flashTicks = 0;
}

bool TurboMeter::fill(float seconds) {
    if (m_held >= kFull) {
        return false;
    }
    m_held = std::min(m_held + kFillPerSecond * seconds, kFull);
    return m_held >= kFull;
}

void TurboMeter::drain(float seconds) {
    m_held = std::max(m_held - kShoveDrainPerSecond * seconds, 0.0f);
}

void TurboMeter::add(float amount) {
    m_held = std::clamp(m_held + amount, 0.0f, kFull);
}

bool TurboMeter::spend(float cost) {
    if (m_held < cost) {
        return false;
    }
    m_held = std::max(m_held - cost, 0.0f);
    return true;
}

TurboMeter::Zone TurboMeter::zoneOf(float amount) {
    const float part = amount / kFull;
    if (part < kHighFrom) {
        return Zone::Low;
    }
    return part < kFullFrom ? Zone::High : Zone::Full;
}

float TurboMeter::zoneFraction(float amount) {
    const float part = amount / kFull;
    switch (zoneOf(amount)) {
    case Zone::Low: return part / kHighFrom;
    case Zone::High: return (part - kHighFrom) / (1.0f - kHighFrom);
    case Zone::Full: break;
    }
    return 1.0f;
}

void TurboMeter::step(std::int32_t ticks) {
    const Zone before = zoneOf(m_shown);
    const auto moved = static_cast<float>(ticks);
    if (m_shown < m_held) {
        m_shown = std::min(m_shown + kRisePerTick * moved, m_held);
    } else {
        m_shown = std::max(m_shown - kFallPerTick * moved, m_held);
    }
    const Zone now = zoneOf(m_shown);
    // A change of zone gleams; full, it glows for as long as it stays so.
    const bool glowDue = now == Zone::Full && m_flash == Flash::None;
    if (before != now || glowDue) {
        m_flash = now == Zone::Full ? Flash::Glow : Flash::Gleam;
        m_flashTicks = 0;
        return;
    }
    m_flashTicks += ticks;
    if (m_flash == Flash::Gleam && m_flashTicks >= 2 * kGleamFrames * kGleamTicksPerFrame) {
        m_flash = Flash::None;
    } else if (m_flash == Flash::Glow && m_flashTicks > kGlowTicks) {
        m_flashTicks = 0;
    }
}

TurboMeterLook TurboMeter::look() const {
    TurboMeterLook look;
    const Zone zone = zoneOf(m_shown);
    const float fraction = zoneFraction(m_shown);
    look.fill = fraction;
    // The front colour brightens through its zone.
    const auto bright = static_cast<std::uint8_t>(127.0f * fraction + 128.0f);
    if (zone == Zone::Low) {
        look.front = Color::rgba(bright, bright, 0);
        look.back = Color::black();
    } else {
        look.front = Color::rgba(bright, 0, 0);
        look.back = Color::rgba(255, 255, 0);
    }
    if (m_flash == Flash::Glow) {
        look.front = Color::rgba(255, 0, 0);
        look.back = look.front;
        // Bright, out, and bright again over one pulse.
        const std::int32_t phase = m_flashTicks * 512 / kGlowTicks;
        const std::int32_t faded = phase <= 255 ? phase : std::max(511 - phase, 0);
        look.glow = static_cast<std::uint8_t>(255 - std::min(faded, 255));
    } else if (m_flash == Flash::Gleam) {
        const std::int32_t step = m_flashTicks / kGleamTicksPerFrame;
        look.gleam = step < kGleamFrames ? step : std::max(2 * kGleamFrames - 1 - step, 0);
    }
    return look;
}

} // namespace gdl::game
