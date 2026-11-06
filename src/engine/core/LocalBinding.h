#pragma once

#include <memory>

#include "engine/core/Types.h"

namespace gdl {

/** The binding a local snapshot is admitted under: one owner, between two rebinds. The owner
 * rebinds wherever its earlier snapshots must stop coming back, as when it binds anew; a move
 * rebinds both sides. Admission only, never peer state, so it is never digested. */
class LocalBinding {
public:
    /** Where a snapshot came from; a default stamp is admitted by no binding. */
    class Stamp {
        friend class LocalBinding;
        std::weak_ptr<const u8> m_token;
        u64 m_epoch = 0;
    };

    LocalBinding() = default;
    LocalBinding(const LocalBinding&) = delete;
    LocalBinding& operator=(const LocalBinding&) = delete;
    /** Allocates the new side's token, so it is not noexcept. */
    LocalBinding(LocalBinding&& other) noexcept(false) { other.rebind(); }
    LocalBinding& operator=(LocalBinding&& other) noexcept {
        rebind();
        other.rebind();
        return *this;
    }
    ~LocalBinding() = default;

    /** Refuses every stamp taken before it. */
    void rebind() noexcept { ++m_epoch; }
    /** Stamps `into` as taken under this binding as it stands. */
    void stamp(Stamp& into) const noexcept {
        into.m_token = m_token;
        into.m_epoch = m_epoch;
    }
    /** Leaves `taken` a stamp that no binding admits. */
    static void unstamp(Stamp& taken) noexcept {
        taken.m_token.reset();
        taken.m_epoch = 0;
    }
    /** Whether this binding took `taken` and has not rebound since. */
    [[nodiscard]] bool admits(const Stamp& taken) const noexcept {
        return taken.m_epoch == m_epoch && taken.m_token.lock() == m_token;
    }

private:
    std::shared_ptr<const u8> m_token = std::make_shared<const u8>(0);
    u64 m_epoch = 1;
};

} // namespace gdl
