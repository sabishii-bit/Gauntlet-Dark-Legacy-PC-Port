#include "engine/audio/MusicDeclicker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

#include "engine/core/Types.h"

namespace gdl {
namespace {
constexpr f64 kWindowSeconds = 0.055;
constexpr f64 kOrderFraction = 0.02;
constexpr f64 kImpulseThreshold = 4;
constexpr f64 kBurstThreshold = 2;
constexpr f64 kPcmScale = 32768;
constexpr usize kMaxErrors = 128;
constexpr f64 kBurstSeconds = 1.0 / 6000;

/** Linear prediction polynomial, found from the window's autocorrelation. */
std::vector<f64> prediction(std::span<const f64> input, usize order, f64& deviation) {
    std::vector<f64> correlation(order + 1, 0);
    for (usize lag = 0; lag <= order; ++lag) {
        for (usize i = lag; i < input.size(); ++i) {
            correlation[lag] += input[i] * input[i - lag];
        }
        correlation[lag] /= static_cast<f64>(input.size());
    }
    f64 error = correlation[0];
    std::vector<f64> polynomial(order + 1, 0);
    polynomial[0] = 1;
    for (usize degree = 1; degree <= order; ++degree) {
        if (!std::isfinite(error) || error <= 1e-20) {
            return {};
        }
        f64 residual = correlation[degree];
        for (usize j = 1; j < degree; ++j) {
            residual += polynomial[j] * correlation[degree - j];
        }
        const f64 reflection = -residual / error;
        const auto previous = polynomial;
        for (usize j = 1; j < degree; ++j) {
            polynomial[j] = previous[j] + reflection * previous[degree - j];
        }
        polynomial[degree] = reflection;
        error *= 1 - reflection * reflection;
    }
    deviation = std::sqrt(error);
    return std::isfinite(deviation) ? polynomial : std::vector<f64>{};
}

/** Solve the positive-definite normal equations without changing the known samples. */
bool solve(std::vector<f64>& matrix, std::vector<f64>& values) {
    const usize count = values.size();
    for (usize column = 0; column < count; ++column) {
        for (usize row = column; row < count; ++row) {
            f64 value = matrix[row * count + column];
            for (usize k = 0; k < column; ++k) {
                value -= matrix[row * count + k] * matrix[column * count + k];
            }
            if (row == column) {
                if (!std::isfinite(value) || value <= 1e-15) {
                    return false;
                }
                matrix[row * count + column] = std::sqrt(value);
            } else {
                matrix[row * count + column] = value / matrix[column * count + column];
            }
        }
    }
    for (usize row = 0; row < count; ++row) {
        for (usize column = 0; column < row; ++column) {
            values[row] -= matrix[row * count + column] * values[column];
        }
        values[row] /= matrix[row * count + row];
    }
    for (usize row = count; row-- > 0;) {
        for (usize column = row + 1; column < count; ++column) {
            values[row] -= matrix[column * count + row] * values[column];
        }
        values[row] /= matrix[row * count + row];
    }
    return std::ranges::all_of(values, [](f64 value) { return std::isfinite(value); });
}
} // namespace

void MusicDeclicker::reset(u32 sampleRate, u32 channels, Pass pass) {
    if (sampleRate < 8000 || sampleRate > 192000 || channels == 0 || channels > 2) {
        throw std::invalid_argument("unsupported music restoration format");
    }
    m_channels = channels;
    m_window = std::max<usize>(100, static_cast<usize>(sampleRate * kWindowSeconds));
    m_hop = m_window / 4;
    m_margin = (m_window - m_hop) / 2;
    m_order = std::max<usize>(1, static_cast<usize>(static_cast<f64>(m_window) * kOrderFraction));
    m_burstLimit = std::max<usize>(1, static_cast<usize>(sampleRate * kBurstSeconds));
    m_pending.assign(m_margin * channels, 0);
    m_frames = m_emitted = 0;
    m_finished = false;
    m_pass = pass;
}

void MusicDeclicker::feed(std::span<const f32> samples, std::vector<f32>& output) {
    if (m_channels == 0 || m_finished || samples.size() % m_channels != 0) {
        throw std::logic_error("music restoration needs complete frames and an active stream");
    }
    m_frames += samples.size() / m_channels;
    m_pending.insert(m_pending.end(), samples.begin(), samples.end());
    process(output, false);
}

void MusicDeclicker::finish(std::vector<f32>& output) {
    if (m_channels == 0 || m_finished) {
        return;
    }
    m_finished = true;
    process(output, true);
}

void MusicDeclicker::process(std::vector<f32>& output, bool finishing) {
    usize consumed = 0;
    std::vector<f64> input(m_window);
    std::vector<f64> restored(m_window);
    while (m_emitted < m_frames) {
        const usize needed = (consumed + m_window) * m_channels;
        if (m_pending.size() < needed) {
            if (!finishing) {
                break;
            }
            m_pending.resize(needed, 0);
        }
        const usize count = std::min(m_hop, m_frames - m_emitted);
        const usize begin = output.size();
        output.resize(begin + count * m_channels);
        for (usize channel = 0; channel < m_channels; ++channel) {
            for (usize frame = 0; frame < m_window; ++frame) {
                input[frame] = m_pending[(consumed + frame) * m_channels + channel];
            }
            restore(input, restored);
            for (usize frame = 0; frame < count; ++frame) {
                const usize absolute = m_emitted + frame;
                const bool hasContext =
                    absolute >= m_order && (!finishing || absolute + m_order < m_frames);
                output[begin + frame * m_channels + channel] =
                    static_cast<f32>((hasContext ? restored : input)[m_margin + frame]);
            }
        }
        m_emitted += count;
        consumed += m_hop;
    }
    m_pending.erase(m_pending.begin(),
                    m_pending.begin() + static_cast<std::ptrdiff_t>(consumed * m_channels));
}

void MusicDeclicker::restore(std::span<const f64> input, std::span<f64> output) const {
    for (usize i = 0; i < input.size(); ++i) {
        output[i] = input[i];
    }
    f64 deviation = 0;
    const auto polynomial = prediction(input, m_order, deviation);
    if (polynomial.empty()) {
        return;
    }
    const f64 threshold = m_pass == Pass::Impulses ? kImpulseThreshold : kBurstThreshold;
    std::vector<usize> errors;
    std::vector<bool> flagged(m_window, false);
    for (usize i = m_order; i < m_window - m_order; ++i) {
        f64 residual = 0;
        for (usize lag = 0; lag <= m_order; ++lag) {
            residual += polynomial[lag] * input[i - lag];
        }
        if (std::abs(residual) > threshold * deviation) {
            errors.push_back(i);
            flagged[i] = true;
        }
    }
    // Dense/noisy windows are not isolated clicks. Bound both work and intervention.
    if (errors.empty() || errors.size() > kMaxErrors) {
        return;
    }
    std::vector<f64> weights(m_order + 1, 0);
    for (usize lag = 0; lag <= m_order; ++lag) {
        for (usize i = lag; i <= m_order; ++i) {
            weights[lag] += polynomial[i] * polynomial[i - lag];
        }
    }
    const usize count = errors.size();
    std::vector<f64> matrix(count * count, 0);
    std::vector<f64> values(count, 0);
    for (usize row = 0; row < count; ++row) {
        const usize at = errors[row];
        for (usize column = 0; column < count; ++column) {
            const usize other = errors[column];
            const usize distance = at > other ? at - other : other - at;
            if (distance <= m_order) {
                matrix[row * count + column] = weights[distance];
            }
        }
        for (usize i = at - m_order; i <= at + m_order; ++i) {
            if (!flagged[i]) {
                values[row] -= input[i] * weights[i > at ? i - at : at - i];
            }
        }
    }
    if (!solve(matrix, values)) {
        return;
    }
    for (usize first = 0; first < count;) {
        usize end = first + 1;
        while (end < count && errors[end] == errors[end - 1] + 1) {
            ++end;
        }
        bool repair = m_pass == Pass::Impulses;
        if (!repair && end - first <= m_burstLimit) {
            // A very short full-scale reversal bounded by the same polarity is a
            // residual burst, not a sustained musical attack. Leave other weak detections alone.
            const f64 left = input[errors[first] - 1];
            const f64 right = input[errors[end - 1] + 1];
            if (left * right > 0 && std::abs(left) > 0.5 && std::abs(right) > 0.5) {
                for (usize i = first; i < end; ++i) {
                    repair = repair || (input[errors[i]] * left < 0 &&
                                        std::abs(input[errors[i]] - values[i]) > 1);
                }
            }
        }
        if (repair) {
            for (usize i = first; i < end; ++i) {
                // Match PCM16 audition precision; unflagged input is never requantized.
                output[errors[i]] =
                    std::clamp(std::nearbyint(values[i] * kPcmScale), -kPcmScale, kPcmScale - 1) /
                    kPcmScale;
            }
        }
        first = end;
    }
}

} // namespace gdl
