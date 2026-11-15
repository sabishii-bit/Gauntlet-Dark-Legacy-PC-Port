#include "game/hud/BossMeter.h"

#include <algorithm>
#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr std::string_view kBackground = "METER_BG";
constexpr std::string_view kFill = "METER_FG";

const Texture* textureOf(TextureSet* textures, RenderDevice& device, std::string_view prefix,
                         std::string_view stem, s32 piece) {
    if (textures == nullptr) {
        return nullptr;
    }
    const std::string name =
        std::format("{}{}{}{}", prefix, prefix.empty() ? "" : "_", stem, piece + 1);
    const auto index = textures->find(name);
    if (!index.has_value()) {
        return nullptr;
    }
    try {
        return &textures->texture(device, *index);
    } catch (const std::exception& e) {
        log::warn("Boss meter: texture {}: {}", name, e.what());
        return nullptr;
    }
}

} // namespace

void BossMeter::saveLocal(LocalSnapshot& saved) const {
    saved.m_values = m_sim;
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
}
bool BossMeter::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner.lock() == m_lifetime && saved.m_epoch == m_localEpoch;
}
bool BossMeter::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_values;
    return true;
}

bool BossMeter::equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const {
    return canRestoreLocal(a) && canRestoreLocal(b) && a.m_values == b.m_values;
}

void BossMeter::appendValuesDigest(NativeStateDigest& digest, const Simulation& values) const {
    const auto& [textures, name, pieces, left, leftInset, rightInset, backed, current, lifetime,
                 epoch] = *this;
    // These layout/resource values cannot change without bind/clear advancing
    // the epoch. A guarded snapshot therefore shares this exact configuration.
    // Lifetime and epoch authenticate local history, not peer-visible state.
    (void)current;
    (void)lifetime;
    (void)epoch;
    digest.string("BossMeter.native.1");
    digest.resource(textures);
    digest.values(name, pieces, left, leftInset, rightInset, backed, values);
}

bool BossMeter::appendNativeDigest(NativeStateDigest& digest) const {
    appendValuesDigest(digest, m_sim);
    return digest.good();
}

bool BossMeter::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    appendValuesDigest(digest, saved.m_values);
    return digest.good();
}

void BossMeters::saveLocal(LocalSnapshot& saved) const {
    for (usize i = 0; i < kCapacity; ++i) {
        m_meters[i].saveLocal(saved.m_meters[i]);
    }
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
}
bool BossMeters::canRestoreLocal(const LocalSnapshot& saved) const {
    if (saved.m_owner.lock() != m_lifetime || saved.m_epoch != m_localEpoch) {
        return false;
    }
    for (usize i = 0; i < kCapacity; ++i) {
        if (!m_meters[i].canRestoreLocal(saved.m_meters[i])) {
            return false;
        }
    }
    return true;
}
bool BossMeters::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize i = 0; i < kCapacity; ++i) {
        m_meters[i].restoreLocal(saved.m_meters[i]);
    }
    return true;
}

bool BossMeters::equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const {
    if (!canRestoreLocal(a) || !canRestoreLocal(b)) {
        return false;
    }
    for (usize i = 0; i < kCapacity; ++i) {
        if (!m_meters[i].equalLocal(a.m_meters[i], b.m_meters[i])) {
            return false;
        }
    }
    return true;
}

bool BossMeters::appendValuesDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    const auto& [meters, count, lifetime, epoch] = *this;
    (void)lifetime;
    (void)epoch;
    digest.string("BossMeters.native.1");
    digest.value(count); // bind-time layout, protected by the collection epoch
    // Include every slot, not just showing layers; an invisible meter still
    // holds fresh/max/easing state used by the next native update.
    for (usize i = 0; i < meters.size(); ++i) {
        const bool appended = saved ? meters[i].appendLocalDigest(digest, saved->m_meters[i])
                                    : meters[i].appendNativeDigest(digest);
        if (!appended) {
            return false;
        }
    }
    return digest.good();
}

bool BossMeters::appendNativeDigest(NativeStateDigest& digest) const {
    return appendValuesDigest(digest, nullptr);
}

bool BossMeters::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendValuesDigest(digest, &saved);
}

bool BossMeter::bind(const HealthMeterDefinition& meter, TextureSet* textures, s32 left) {
    clear();
    if (!meter.shown || meter.pieces <= 0) {
        return false;
    }
    m_textures = textures;
    m_name = meter.name;
    m_pieces = std::min(meter.pieces, kMostPieces);
    m_left = left;
    m_leftInset = meter.leftInset;
    m_rightInset = meter.rightInset;
    m_backed = meter.backed;
    return true;
}

void BossMeter::clear() {
    ++m_localEpoch;
    m_textures = nullptr;
    m_name.clear();
    m_pieces = 0;
    m_left = 0;
    m_leftInset = 0;
    m_rightInset = 0;
    m_backed = false;
    m_sim.max = 1.0f;
    m_sim.shown = 0.0f;
    m_sim.fresh = true;
    m_sim.alive = true;
    m_sim.frozen = false;
}

void BossMeter::update(s32 ticks, f32 health, f32 maxHealth, bool alive, bool frozen) {
    if (!bound()) {
        return;
    }
    m_sim.max = std::max(maxHealth, 1.0f);
    m_sim.alive = alive;
    m_sim.frozen = frozen;
    const f32 target = std::max(health, 0.0f);
    if (m_sim.fresh) {
        m_sim.fresh = false; // first sight: as it is
        m_sim.shown = target;
        return;
    }
    const f32 step = kEasePerTick * static_cast<f32>(std::max(ticks, 0));
    if (m_sim.shown > target) {
        m_sim.shown = std::max(m_sim.shown - step, target);
    } else if (m_sim.shown < target) {
        m_sim.shown = std::min(m_sim.shown + step, target);
    }
}

/** The original's arithmetic: two strips share the health, the first's fill running from
 * its cap to its end over the first half, the second's from its start to its tail over the
 * rest; one strip runs the whole width less both. */
std::array<s32, BossMeter::kMostPieces> BossMeter::fillWidths() const {
    std::array<s32, kMostPieces> widths{};
    if (!bound()) {
        return widths;
    }
    const f32 fraction = std::clamp(m_sim.shown / m_sim.max, 0.0f, 1.0f);
    if (m_pieces >= 2) {
        const f32 share = 2.0f * fraction;
        widths[0] = share >= 1.0f
                        ? kPieceWidth
                        : static_cast<s32>(share * static_cast<f32>(kPieceWidth - m_leftInset) +
                                           static_cast<f32>(m_leftInset));
        widths[1] = static_cast<s32>(std::max(share - 1.0f, 0.0f) *
                                     static_cast<f32>(kPieceWidth - m_rightInset));
    } else {
        widths[0] =
            static_cast<s32>(fraction * static_cast<f32>(kPieceWidth - m_leftInset - m_rightInset));
    }
    return widths;
}

void BossMeter::draw(Canvas& canvas, RenderDevice& device) const {
    draw(canvas, device, look());
}

const Texture* BossMeter::pieceTexture(RenderDevice& device, MeterPart part, s32 piece) const {
    if (piece < 0 || piece >= m_pieces || (part == MeterPart::Background && !m_backed)) {
        return nullptr;
    }
    return textureOf(m_textures, device, m_name,
                     part == MeterPart::Background ? kBackground : kFill, piece);
}

void BossMeter::preload(RenderDevice& device) const {
    for (s32 piece = 0; piece < m_pieces; ++piece) {
        pieceTexture(device, MeterPart::Background, piece);
        pieceTexture(device, MeterPart::Fill, piece);
    }
}

void BossMeter::draw(Canvas& canvas, RenderDevice& device, const Look& look) const {
    if (!bound() || !look.visible) {
        return;
    }
    const Color plain = Color::rgba(255, 255, 255, kAlpha);
    for (s32 piece = 0; piece < m_pieces; ++piece) {
        const auto x = static_cast<f32>(m_left + piece * kPieceWidth);
        if (const Texture* back = pieceTexture(device, MeterPart::Background, piece)) {
            canvas.draw(*back,
                        Rect{x, static_cast<f32>(kY), static_cast<f32>(kPieceWidth),
                             static_cast<f32>(back->height())},
                        look.frozen ? kFrozenTint : plain);
        }
        const s32 width = std::clamp(look.widths[static_cast<usize>(piece)], 0, kPieceWidth);
        if (width <= 0) {
            continue;
        }
        if (const Texture* fill = pieceTexture(device, MeterPart::Fill, piece)) {
            // Cropped, not squeezed: the fill's left `width` pixels.
            const f32 across = static_cast<f32>(width) / static_cast<f32>(kPieceWidth);
            canvas.draw(*fill,
                        Rect{x, static_cast<f32>(kY), static_cast<f32>(width),
                             static_cast<f32>(fill->height())},
                        Rect{0.0f, 0.0f, across, 1.0f}, plain);
        }
    }
}

void BossMeters::bind(std::span<const HealthMeterReading> readings, TextureSet* textures) {
    clear();
    s32 left = 0;
    s32 nextLeft = 0;
    for (const auto& reading : readings) {
        if (!drawsReading(reading)) {
            continue;
        }
        if (m_count == kCapacity) {
            log::warn("Boss meters: more than {} enabled meters", kCapacity);
            break;
        }
        if (reading.definition.backed) {
            left = nextLeft;
            nextLeft += reading.definition.advance;
        }
        auto& meter = m_meters[m_count++];
        meter.bind(reading.definition, textures, left);
        meter.update(0, reading.health, reading.maxHealth, true, false);
    }
}

void BossMeters::update(s32 ticks, std::span<const HealthMeterReading> readings, bool alive,
                        bool frozen) {
    usize index = 0;
    for (const auto& reading : readings) {
        if (!drawsReading(reading)) {
            continue;
        }
        if (index == m_count) {
            break;
        }
        m_meters[index++].update(ticks, reading.health, reading.maxHealth, alive, frozen);
    }
    // Missing actors may have been retired this frame; never leave a stale layer visible.
    while (index < m_count) {
        m_meters[index++].update(0, 0, 1, false, false);
    }
}

void BossMeters::clear() {
    ++m_localEpoch;
    for (auto& meter : m_meters) {
        meter.clear();
    }
    m_count = 0;
}

void BossMeters::draw(Canvas& canvas, RenderDevice& device) const {
    for (usize index = 0; index < m_count; ++index) {
        m_meters[index].draw(canvas, device);
    }
}

} // namespace gdl::game
