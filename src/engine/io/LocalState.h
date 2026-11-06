#pragma once

#include <tuple>
#include <type_traits>

#include "engine/core/LocalBinding.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/NativeStateDigest.h"

namespace gdl {

/** What a LocalState owner declares about its admission and restore. An answer has no default to
 * fall back on, so traits that leave one out do not compile: an owner states each one. */
struct LocalTraits {
    /** One trait's answer, written as a bool. */
    class Answer {
    public:
        constexpr Answer(bool holds) : m_holds(holds) {}
        constexpr explicit operator bool() const { return m_holds; }

    private:
        bool m_holds;
    };

    Answer refusalInvalidates; ///< a refusal invalidates the digest it was asked to extend
    Answer gatesOnBound;       ///< saves and admits only while the owner's localBound() holds
    Answer gatesOnValues;      ///< admits only values the owner's admitsLocalValues() accepts
    Answer notifiesRestore;    ///< calls the owner's restoredLocal() after every restore
};

/**
 * Saves, admits, compares, restores and hashes an owner whose own values are one aggregate,
 * composing the children its roster names, in digest order for every operation. The owner keeps
 * its m_localBinding and names its values; this base holds nothing.
 *
 * The owner derives from LocalState<Owner> and befriends it. The base declares none of what the
 * owner names, so each is required: kLocalTraits; LocalValues; LocalChildren, where a snapshot
 * keeps its children's; localParts(), its roster, naming own() once; localValues(), const and not;
 * digestLocalValues(digest, values); m_localBinding; and the hook of each trait it turns on:
 * localBound(), admitsLocalValues(values) and restoredLocal().
 */
template <class Owner> class LocalState {
public:
    class LocalSnapshot;

    /** Saves the owner's values and children into `saved` under its binding. False, leaving
     * `saved` unstamped and so admitted by nobody, when the owner is unbound or a child's save
     * fails. */
    bool saveLocal(LocalSnapshot& saved) const;
    /** Whether the owner's binding stamped `saved` and every part admits it. */
    [[nodiscard]] bool canRestoreLocal(const LocalSnapshot& saved) const;
    /** Takes `saved` back, part by part in roster order, once the whole of it is admitted. */
    bool restoreLocal(const LocalSnapshot& saved);
    /** Whether two snapshots the owner admits hold equal values and children. */
    [[nodiscard]] bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    /** Never consults localBound(): the live digest is written whatever the owner's binding. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    /** Writes `saved` as the owner's live digest was written when it was saved; a refused
     * snapshot writes nothing. */
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    friend Owner;
    LocalState() = default;

    /** How many times `parts` names the owner's own values. */
    template <class... Part> static constexpr int ownCount(const std::tuple<Part...>& /*parts*/) {
        return (0 + ... + (std::is_same_v<Part, local::Own> ? 1 : 0));
    }
    /** The owner's roster, which names its own values once. */
    static constexpr auto roster() {
        static_assert(ownCount(Owner::localParts()) == 1, "A LocalState roster names own() once");
        return Owner::localParts();
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): only an Owner builds it
    const Owner& self() const { return static_cast<const Owner&>(*this); }
    Owner& self() { return static_cast<Owner&>(*this); }
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
};

/** What an owner saved: its values, its children's snapshots and the stamp of the binding it was
 * saved under, which admits it and is never digested. */
template <class Owner> class LocalState<Owner>::LocalSnapshot {
    friend class LocalState<Owner>;
    friend Owner;
    typename Owner::LocalValues m_values;
    typename Owner::LocalChildren m_children;
    LocalBinding::Stamp m_stamp;
};

template <class Owner> bool LocalState<Owner>::saveLocal(LocalSnapshot& saved) const {
    LocalBinding::unstamp(saved.m_stamp);
    const Owner& owner = self();
    if constexpr (Owner::kLocalTraits.gatesOnBound) {
        if (!owner.localBound()) {
            return false;
        }
    }
    if (!local::save(owner, roster(), saved.m_children,
                     [&] { saved.m_values = owner.localValues(); })) {
        return false;
    }
    owner.m_localBinding.stamp(saved.m_stamp);
    return true;
}

template <class Owner> bool LocalState<Owner>::canRestoreLocal(const LocalSnapshot& saved) const {
    const Owner& owner = self();
    if (!owner.m_localBinding.admits(saved.m_stamp)) {
        return false;
    }
    if constexpr (Owner::kLocalTraits.gatesOnBound) {
        if (!owner.localBound()) {
            return false;
        }
    }
    return local::admits(owner, roster(), saved.m_children, [&] {
        if constexpr (Owner::kLocalTraits.gatesOnValues) {
            return owner.admitsLocalValues(saved.m_values);
        } else {
            return true;
        }
    });
}

template <class Owner> bool LocalState<Owner>::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    Owner& owner = self();
    local::restore(owner, roster(), saved.m_children,
                   [&] { owner.localValues() = saved.m_values; });
    if constexpr (Owner::kLocalTraits.notifiesRestore) {
        owner.restoredLocal();
    }
    return true;
}

template <class Owner>
bool LocalState<Owner>::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) &&
           local::equal(self(), roster(), left.m_children, right.m_children,
                        [&] { return left.m_values == right.m_values; });
}

template <class Owner> bool LocalState<Owner>::appendNativeDigest(NativeStateDigest& digest) const {
    const Owner& owner = self();
    const typename Owner::LocalChildren* const live = nullptr;
    return local::append(owner, roster(), digest, live,
                         [&] { return owner.digestLocalValues(digest, owner.localValues()); });
}

template <class Owner>
bool LocalState<Owner>::appendLocalDigest(NativeStateDigest& digest,
                                          const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        if constexpr (Owner::kLocalTraits.refusalInvalidates) {
            digest.invalidate();
        }
        return false;
    }
    const Owner& owner = self();
    return local::append(owner, roster(), digest, &saved.m_children,
                         [&] { return owner.digestLocalValues(digest, saved.m_values); });
}

} // namespace gdl
