#pragma once

#include "game/netplay/MatchSession.h"

namespace gdl::test {

/** Input-only diagnostic consumer for MatchSession lifecycle tests. It exercises
 * the real rollback admission/timeline, not a second shipping simulation. Tests
 * which mutate gameplay during predicted ticks must use NativeRollbackDriver. */
class RollbackSessionHarness {
public:
    bool bind(game::MatchSession& session) {
        clear();
        const auto& context = session.context();
        if (session.phase() != game::MatchSession::Phase::Loading ||
            !m_inputs.open(session.localPeer(), context.epoch, context.owners, context.grants,
                           context.inputLead)) {
            return false;
        }
        m_session = &session;
        m_epoch = context.epoch;
        if (!session.attachNativeInputs(m_epoch, [this](
                                                     const game::RollbackExchange::Message& message,
                                                     std::optional<u8> transit) {
                const auto accepted = [](game::RollbackInputs::Admission result) {
                    using A = game::RollbackInputs::Admission;
                    return result == A::Accepted || result == A::Duplicate || result == A::Stale;
                };
                if (!message.commands.empty() &&
                    !accepted(m_inputs.submit(message.origin, message.commands))) {
                    return false;
                }
                return !transit || accepted(m_inputs.observePeerTick(message.origin, message.epoch,
                                                                     message.nextTick, *transit));
            })) {
            m_session = nullptr;
            return false;
        }
        return true;
    }
    void clear() {
        if (m_session) {
            m_session->detachNativeInputs(m_epoch);
        }
        m_session = nullptr;
        m_epoch = 0;
        m_inputs = {};
        m_lastFrame.reset();
    }
    game::RollbackInputs& inputs() { return m_inputs; }
    bool sample(std::span<const game::InputCommand> commands) {
        return running() && m_inputs.sample(commands) && publish();
    }
    bool publish() {
        return running() &&
               m_session->publishNativeInputs(m_inputs.redundantInputs(), m_inputs.tick());
    }
    std::optional<game::RollbackInputs::Frame> advance() {
        // As the shipping loop does before every advance.
        if (!running() || !m_session->reportHostReceipts(m_inputs)) {
            return std::nullopt;
        }
        if (const auto wrong = m_inputs.mismatchTick()) {
            const auto head = m_inputs.tick();
            if (!m_inputs.rewind(*wrong)) {
                return std::nullopt;
            }
            while (m_inputs.tick() < head) {
                const auto replayed = m_inputs.advance();
                if (!replayed) {
                    return std::nullopt;
                }
                m_lastFrame = replayed;
            }
        }
        if (m_inputs.timeSyncIdle() ||
            (m_session->nativeEndTick() && m_inputs.tick() > *m_session->nativeEndTick())) {
            return std::nullopt;
        }
        const auto frame = m_inputs.advance();
        if (frame) {
            m_lastFrame = frame;
        }
        return frame;
    }
    bool complete() {
        return publish() && m_session->confirmNativeTick(m_epoch, m_inputs.confirmedTick());
    }
    bool completeBoundary() {
        return complete() && m_session->completeNativeBoundary(m_epoch, m_inputs.tick() - 1);
    }
    /** Diagnostic certificate for input-only lifecycle tests, not a scene hash.
     * Tests explicitly submit every peer and pump agreement before marking a
     * boundary. Native gameplay tests use the driver's complete-state digest. */
    bool submitBoundaryDigest(game::MatchSession::Time now = game::MatchSession::Clock::now()) {
        if (!complete()) {
            return false;
        }
        game::NativeAgreement::Digest digest{};
        const u64 tick = m_inputs.tick() - 1;
        for (u32 byte = 0; byte < 8; ++byte) {
            digest[byte] = static_cast<u8>(tick >> (byte * 8));
        }
        return m_session->observeNativeTerminal(m_epoch, tick, digest, now);
    }
    bool sampleReady() const { return running() && m_inputs.sampleReady(); }
    u64 tick() const { return m_inputs.tick(); }
    u64 sampleTick() const { return m_inputs.sampleTick(); }
    u64 confirmedTick() const { return m_inputs.confirmedTick(); }
    const game::RollbackInputs& inputs() const { return m_inputs; }
    const std::optional<game::RollbackInputs::Frame>& lastFrame() const { return m_lastFrame; }

private:
    bool running() const {
        return (m_session != nullptr) && m_session->context().epoch == m_epoch &&
               m_session->phase() == game::MatchSession::Phase::Running;
    }
    game::MatchSession* m_session = nullptr;
    u64 m_epoch = 0;
    game::RollbackInputs m_inputs;
    std::optional<game::RollbackInputs::Frame> m_lastFrame;
};
} // namespace gdl::test
