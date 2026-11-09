#include "expansions/n64/level/LegendsTowerCeremony.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <random>
#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr f32 kTick = 1.0f / 30;
constexpr std::array<u8, 4> kDoorRequirements{0x60, 7, 0x18, 0x80};
bool captionHasText(u32 id) {
    return id != 0 && id != 521 && id != 522 && id != 526;
}
std::vector<u32> captions(std::span<const u16> ids) {
    std::vector<u32> result;
    for (const auto id : ids) {
        if (captionHasText(id)) {
            result.push_back(id);
        }
    }
    return result;
}
} // namespace

LegendsTowerCeremony LegendsTowerCeremony::decode(std::span<const u8> overlay) {
    LegendsTowerCeremony result;
    result.m_native = formats::decodeLegendsN64Tower(overlay); // Revision authentication.
    const auto vector = [&](usize at) {
        return Vec3{std::bit_cast<f32>(readU32BE(overlay, at)),
                    std::bit_cast<f32>(readU32BE(overlay, at + 4)),
                    std::bit_cast<f32>(readU32BE(overlay, at + 8))};
    };
    result.m_runeFocus = vector(0x967c) + Vec3{0, 5, 0};
    result.m_shardFocus = vector(0x9670) + Vec3{0, 5, 0};
    constexpr std::array<usize, 4> kDoorRows{3, 1, 2, 4};
    for (usize i = 0; i < 13; ++i) {
        result.m_runeCaptions[i] = readU16BE(overlay, 0x9700 + 2 * i);
    }
    for (usize i = 0; i < 4; ++i) {
        result.m_doorFocus[i] = vector(0x9634 + kDoorRows[i] * 12) + Vec3{0, 5, 0};
        for (usize line = 0; line < 2; ++line) {
            result.m_shardCaptions[i][line] = readU16BE(overlay, 0x96ec + 4 * i + 2 * line);
            result.m_doorCaptions[i][line] =
                readU16BE(overlay, 0x96d8 + 4 * kDoorRows[i] + 2 * line);
        }
    }
    result.m_windowCaptions = {readU16BE(overlay, 0x96fc), readU16BE(overlay, 0x96fe)};
    return result;
}

void LegendsTowerCeremony::bindSequences(std::vector<Sequence> sequences) {
    m_sequences = std::move(sequences);
}

void LegendsTowerCeremony::begin(const LegendsProgress& before, const LegendsProgress& after) {
    m_started = false;
    m_entries.clear();
    m_next = 0;
    m_displayed = after;
    m_owned = after;
    m_captions.clear();
    m_sounds.clear();
    m_windowReveal = (before.shards & 15U) == 15U ? 1 : 0;
    if (!before.valid() || !after.valid()) {
        return;
    }
    m_displayed.runestones &= before.runestones;
    m_displayed.shards &= before.shards;
    m_displayed.obelisks &= before.obelisks;
    auto count = static_cast<usize>(std::popcount(before.runestones & 0x1fffU));
    // e0862220 chooses the lowest pending rune. e08653bc's final assignment
    // gives Castle, Battlefield, Ice, Town priority. e0863364 takes the highest
    // pending shard. Keep this order when several players bring pieces home.
    for (usize i = 0; i < 13; ++i) {
        if ((after.runestones & ~before.runestones & (u64{1} << i)) != 0) {
            m_entries.push_back({Kind::Rune, i, {m_runeCaptions[count++]}});
        }
    }
    for (const usize i : {1U, 3U, 2U, 0U}) {
        const auto required = kDoorRequirements[i];
        if ((after.obelisks & required) == required && (before.obelisks & required) != required) {
            m_entries.push_back({Kind::Door, i, captions(m_doorCaptions[i])});
        }
    }
    for (usize i = 4; i-- > 0;) {
        if ((after.shards & ~before.shards & (u64{1} << i)) != 0) {
            m_entries.push_back({Kind::Shard, i, captions(m_shardCaptions[i])});
        }
    }
    if (!pending()) {
        m_displayed = after;
    }
}

const LegendsTowerCeremony::Entry* LegendsTowerCeremony::current() const {
    return pending() ? &m_entries[m_next] : nullptr;
}

void LegendsTowerCeremony::start(const WorldCamera& camera, const Vec3& attention) {
    if (!pending() || active()) {
        return;
    }
    m_started = true;
    m_remainder = 0;
    m_camera = m_previousCamera = m_returnCamera = camera;
    m_cameraFocus = m_returnFocus = attention;
    shot(m_native.sumner.position + Vec3{0, 3, 0}, 30, kPi / 6);
    next();
}

void LegendsTowerCeremony::play(std::string_view name, bool reverse, f32 speed) {
    m_gesture = name;
    const auto found = std::ranges::find(m_sequences, name, &Sequence::name);
    m_frameEnd = found == m_sequences.end() ? 0 : static_cast<f32>(std::max(0, found->frames - 1));
    const f32 period = found == m_sequences.end() ? kTick : found->secondsPerFrame;
    m_frame = reverse ? m_frameEnd : 0;
    m_frameStep = (reverse ? -1 : 1) * speed * kTick / std::max(period, .0001f);
}

bool LegendsTowerCeremony::advanceGesture() {
    m_frame = std::clamp(m_frame + m_frameStep, 0.0f, m_frameEnd);
    return m_frameStep < 0 ? m_frame <= 0 : m_frame >= m_frameEnd;
}

void LegendsTowerCeremony::queueCaptions(std::span<const u32> ids) {
    if (m_captions.empty()) {
        m_captionSeconds = 0;
    }
    for (const u32 id : ids) {
        if (captionHasText(id)) {
            m_captions.push_back(id);
        }
    }
}

void LegendsTowerCeremony::next() {
    m_phase = 0;
    m_ticks = 0;
    m_flightFrame = -1;
    play("REDY");
    if (const auto* entry = current()) {
        queueCaptions(entry->captions);
        if (entry->kind == Kind::Rune) {
            makeRuneFlight(entry->index);
        }
    } else {
        // e086736c cases 40/41 restore the ordinary camera before handing back
        // control. Ownership was committed before the ceremony began.
        m_phase = 100;
        m_cameraFrom = m_camera;
        m_cameraGoal = m_returnCamera;
        m_focusFrom = m_cameraFocus;
        m_focusGoal = m_returnFocus;
        m_cameraSeconds = 0;
    }
}

void LegendsTowerCeremony::finishEntry() {
    ++m_next;
    next();
}

void LegendsTowerCeremony::tick() {
    m_previousCamera = m_camera;
    updateCamera();
    m_captionSeconds += kTick;
    if (!m_captions.empty() && m_captionSeconds >= 3) {
        m_captions.erase(m_captions.begin());
        m_captionSeconds -= 3;
    }
    if (m_phase == 100) {
        if (++m_ticks >= 54 && m_captions.empty()) {
            m_displayed = m_owned;
            m_started = false;
        }
        return;
    }
    const auto* entry = current();
    if (!entry) {
        return;
    }
    const bool gestureDone = m_phase == 5 && entry->kind == Kind::Door ? false : advanceGesture();
    ++m_ticks;
    if (entry->kind == Kind::Rune) {
        // e086239c: camera at 20, MKRU at 50 (half speed), release at frame 14,
        // reverse the 62-sample native falling/tumbling path, then wait 30 ticks.
        if (m_phase == 0 && m_ticks == 20) {
            shot(m_runeFocus, 30, 25 * kPi / 180);
        }
        if (m_phase == 0 && m_ticks >= 50) {
            play("MKRU", false, .5f);
            m_sounds.push_back(0x68);
            m_phase = 1;
        } else if (m_phase == 1 && (m_frame >= 14 || gestureDone)) {
            m_flightFrame = 0;
            m_phase = 2;
        } else if (m_phase == 2) {
            ++m_flightFrame;
            if (gestureDone && m_gesture != "REDY") {
                play("REDY");
            }
            if (m_flightFrame >= static_cast<s32>(m_flight.size())) {
                m_displayed.runestones |= u64{1} << entry->index;
                m_flightFrame = -1;
                m_sounds.push_back(0x8a);
                m_ticks = 0;
                m_phase = 3;
            }
        } else if (m_phase == 3 && m_ticks >= 30) {
            finishEntry();
        }
        return;
    }
    if (entry->kind == Kind::Shard) {
        // e0863440: all waits count native 30 Hz updates, not rendered frames.
        if (m_phase == 0 && m_ticks >= 41) {
            shot(m_shardFocus, 30, 10 * kPi / 180);
            m_phase = 1;
            m_ticks = 0;
        } else if (m_phase == 1 && m_ticks >= 51) {
            play("SHD1");
            m_phase = 2;
        } else if (m_phase == 2 && gestureDone) {
            m_displayed.shards |= u64{1} << entry->index;
            m_phase = 3;
            m_ticks = 0;
        } else if (m_phase == 3 && m_ticks >= 21) {
            m_sounds.push_back(0x68);
            m_phase = 4;
            m_ticks = 0;
        } else if (m_phase == 4 && m_ticks >= 25) {
            play("SHDR");
            m_ticks = 0;
            m_phase = (m_displayed.shards & 15U) == 15U ? 6 : 5;
            if (m_phase == 6) {
                shot(m_shardFocus - Vec3{0, 7, 0}, 38, kPi / 6);
                const auto lines = captions(m_windowCaptions);
                queueCaptions(lines);
            }
        } else if (m_phase == 5 && m_ticks >= 41) {
            finishEntry();
        } else if (m_phase == 6 && m_ticks >= 51) {
            m_phase = 7;
            m_ticks = 0;
        } else if (m_phase == 7) {
            m_windowReveal = std::min(static_cast<f32>(m_ticks) / 40, 1.0f);
            if (m_ticks >= 80) {
                m_phase = 5;
                m_ticks = 0;
            }
        }
        return;
    }
    // e0865c98: STRT reverses then plays forward; ODOR holds frame 15 for 50
    // updates before releasing the door at frame 19. No proximity unlock.
    if (m_phase == 0 && m_ticks >= 41) {
        shot(m_doorFocus[entry->index], 30, 25 * kPi / 180);
        m_phase = 1;
        m_ticks = 0;
    } else if (m_phase == 1 && m_ticks >= 6) {
        play("STRT", true);
        m_ticks = 0;
        m_phase = 2;
    } else if (m_phase == 2 && gestureDone) {
        play("STRT");
        m_phase = 3;
    } else if (m_phase == 3 && gestureDone) {
        play("ODOR");
        m_phase = 4;
    } else if (m_phase == 4 && (m_frame >= 15 || gestureDone)) {
        m_frame = std::min(15.0f, m_frameEnd);
        m_phase = 5;
        m_ticks = 0;
    } else if (m_phase == 5 && m_ticks >= 50) {
        m_phase = 6;
        m_sounds.push_back(0x6e);
    } else if (m_phase == 6 && (m_frame > 19 || gestureDone)) {
        m_displayed.obelisks |= kDoorRequirements[entry->index];
        m_phase = 7;
        m_ticks = 0;
    } else if (m_phase == 7 && m_ticks > 40) {
        finishEntry();
    }
    if (m_phase == 2 && m_frameEnd - m_frame >= 30 && m_frameEnd - m_frame + m_frameStep < 30) {
        m_sounds.push_back(0xb9);
    }
}

void LegendsTowerCeremony::update(f32 seconds) {
    if (!active() || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_remainder += std::min(seconds, 1.0f);
    while (m_remainder + 1e-9 >= kTick && active()) {
        m_remainder -= kTick;
        tick();
    }
}

void LegendsTowerCeremony::shot(const Vec3& focus, f32 distance, f32 pitch) {
    m_cameraFrom = m_camera;
    m_focusFrom = m_cameraFocus;
    m_focusGoal = focus;
    m_cameraGoal = m_camera;
    m_cameraGoal.pitch = pitch;
    m_cameraGoal.roll = 0;
    m_cameraGoal.position = focus - m_cameraGoal.forward() * distance;
    m_cameraSeconds = 0;
}

void LegendsTowerCeremony::updateCamera() {
    // e08694e4 configures acceleration .7, cruise 1 and braking .3 seconds;
    // e086736c changes the acceleration to .5 for the return to gameplay.
    // Integrate that trapezoidal speed profile, rather than snapping shots.
    const f32 acceleration = m_phase == 100 ? .5f : .7f;
    const f32 cruiseEnd = acceleration + 1;
    m_cameraSeconds = std::min(m_cameraSeconds + kTick, cruiseEnd + .3f);
    const f32 t = m_cameraSeconds;
    const f32 distance = t < acceleration ? t * t / (2 * acceleration)
                         : t < cruiseEnd  ? acceleration * .5f + t - acceleration
                                          : acceleration * .5f + 1 + (t - cruiseEnd) -
                                               (t - cruiseEnd) * (t - cruiseEnd) / .6f;
    const f32 blend = std::clamp(distance / ((acceleration + .3f) * .5f + 1), 0.0f, 1.0f);
    m_camera = m_cameraGoal.interpolate(m_cameraFrom, blend);
    m_cameraFocus = glm::mix(m_focusFrom, m_focusGoal, blend);
}

WorldCamera LegendsTowerCeremony::camera(f32 alpha) const {
    return alpha < 0 ? m_camera
                     : m_camera.interpolate(m_previousCamera, std::clamp(alpha, 0.0f, 1.0f));
}

void LegendsTowerCeremony::makeRuneFlight(usize index) {
    // e08618a8 records 62 poses starting at the final floor placement, then
    // e086239c reads the recording backwards. This is not a hand-to-floor lerp.
    const auto& final = m_native.runestones[index].placement;
    FlightPose pose{Vec3{final[3]}, {kHalfPi, 0, 0}};
    const Vec3 centre = m_runeFocus - Vec3{0, 5, 0};
    pose.angles.y = std::atan2(centre.x - pose.position.x, centre.z - pose.position.z);
    std::minstd_rand random{static_cast<u32>(index + 1)};
    Vec3 spin;
    for (s32 axis = 0; axis < 3; ++axis) {
        do {
            spin[axis] = .08f - static_cast<f32>(random() % 32768) * .16f / 32768;
        } while (spin[axis] < .012f);
    }
    Vec3 velocity = -2.0f * spin;
    const Vec3 acceleration = -.25f * velocity;
    f32 ySpeed = .1f;
    m_flight[0] = pose;
    usize at = 1;
    for (s32 i = 0; i < 8; ++i) {
        velocity += acceleration;
        pose.angles += velocity;
        ySpeed -= .025f;
        pose.position.y += ySpeed;
        m_flight[at++] = pose;
    }
    for (s32 i = 0; i < 3; ++i) {
        pose.position.y += .1f;
        pose.angles += spin;
        m_flight[at++] = pose;
    }
    velocity = Vec3{0};
    ySpeed = .7f;
    for (s32 i = 0; i < 50; ++i) {
        velocity = glm::clamp(velocity + spin, Vec3{-kPi / 6}, Vec3{kPi / 6});
        ySpeed = std::max(0.0f, ySpeed - .04f);
        pose.position.y += ySpeed;
        pose.angles += velocity;
        m_flight[at++] = pose;
    }
}

Mat4 LegendsTowerCeremony::runePlacement() const {
    if (!current() || current()->kind != Kind::Rune || m_flightFrame < 0) {
        return Mat4{1};
    }
    const auto& pose = m_flight[61 - std::min(m_flightFrame, 61)];
    return glm::translate(Mat4{1}, pose.position) *
           glm::rotate(Mat4{1}, pose.angles.y, Vec3{0, 1, 0}) *
           glm::rotate(Mat4{1}, -pose.angles.x, Vec3{1, 0, 0}) *
           glm::rotate(Mat4{1}, -pose.angles.z, Vec3{0, 0, 1}) * glm::scale(Mat4{1}, Vec3{1.8f});
}

f32 LegendsTowerCeremony::runeOpacity() const {
    return std::clamp(static_cast<f32>(m_flightFrame) * kTick / .8f, 0.0f, 1.0f);
}
f32 LegendsTowerCeremony::shardOpacity() const {
    if (m_phase == 3) {
        return 0;
    }
    if (m_phase == 4) {
        const auto native = static_cast<u8>((m_ticks - 4) * 208 / 20);
        return static_cast<f32>(255 - native) / 255;
    }
    return (47.0f + 208 * m_windowReveal) / 255;
}
f32 LegendsTowerCeremony::shardWhiteBlend() const {
    return m_phase == 3 ? 1
           : m_phase == 4
               ? 1 - std::clamp(static_cast<f32>((m_ticks - 4) * (m_ticks - 4)) / 400, 0.0f, 1.0f)
               : 0;
}
std::optional<u32> LegendsTowerCeremony::caption() const {
    return m_captions.empty() ? std::nullopt : std::optional{m_captions.front()};
}
f32 LegendsTowerCeremony::captionOpacity() const {
    return static_cast<f32>(
        std::clamp(std::min(m_captionSeconds, 3 - m_captionSeconds) / .3, 0.0, 1.0));
}
std::vector<u32> LegendsTowerCeremony::takeSounds() {
    return std::exchange(m_sounds, {});
}

} // namespace gdl::game
