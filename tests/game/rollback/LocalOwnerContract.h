#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

namespace gdl::test {

/** How a snapshot owner answers a snapshot it is asked to take back. */
enum class Admission : u8 {
    Admitted,             ///< canRestoreLocal, restoreLocal and appendLocalDigest take it
    Refused,              ///< all three refuse, and the digest asked to take it is spoilt
    RefusedKeepingDigest, ///< all three refuse, yet the digest asked to take it stays good
    Unasked,              ///< the owner has no such action
};

/** A snapshot owner's admission rules, as its row shows them when run. */
struct LocalOwnerRules {
    Admission foreign = Admission::Unasked; ///< saved by another live owner of the same kind
    Admission blank = Admission::Unasked;   ///< a snapshot nobody saved
    Admission cleared = Admission::Unasked; ///< saved, then the owner cleared or closed
    /** Saved and restored once, then the owner cleared or closed. */
    Admission restoredThenCleared = Admission::Unasked;
    Admission rebound = Admission::Unasked;   ///< saved, then the owner bound or opened anew
    Admission movedInto = Admission::Unasked; ///< saved, then the owner moved into a new one
    /** Saved, then the owner moved from: only whether it takes the snapshot, since what a
     * moved-from owner holds is unspecified. */
    Admission movedFrom = Admission::Unasked;
    /** Saved, then the owner move-assigned over another live owner, which is asked. */
    Admission assignedInto = Admission::Unasked;
    Admission copied = Admission::Unasked; ///< saved, then a copy of the owner asked
    bool advances = false;                 ///< the row's advance changes the live digest
    /** Saved, advanced and restored, the live digest is the one the snapshot hashes to. */
    bool roundTrips = false;
    bool operator==(const LocalOwnerRules&) const = default;
};

/** Writes an answer as the enumerator a row records it with. */
inline std::string_view nameOf(Admission answer) {
    switch (answer) {
    case Admission::Admitted: return "Admitted";
    case Admission::Refused: return "Refused";
    case Admission::RefusedKeepingDigest: return "RefusedKeepingDigest";
    case Admission::Unasked: return "Unasked";
    }
    return "?";
}

/** Writes an answer by its enumerator's name, as a failing check shows it. */
inline std::ostream& operator<<(std::ostream& out, Admission answer) {
    return out << nameOf(answer);
}

/** Writes rules as the designated initializer a row records them with. */
inline std::ostream& operator<<(std::ostream& out, const LocalOwnerRules& rules) {
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    return out << "{.foreign = " << nameOf(rules.foreign) << ", .blank = " << nameOf(rules.blank)
               << ", .cleared = " << nameOf(rules.cleared)
               << ", .restoredThenCleared = " << nameOf(rules.restoredThenCleared)
               << ", .rebound = " << nameOf(rules.rebound)
               << ", .movedInto = " << nameOf(rules.movedInto)
               << ", .movedFrom = " << nameOf(rules.movedFrom)
               << ", .assignedInto = " << nameOf(rules.assignedInto)
               << ", .copied = " << nameOf(rules.copied) << ", .advances = " << flag(rules.advances)
               << ", .roundTrips = " << flag(rules.roundTrips) << "}";
}

/** Whether a copy of `Owner` is asked about the snapshot its source saved: set for the owners
 * whose identity a copy shares, so that what a copy answers is recorded with their rules. */
template <class Owner> inline constexpr bool kCopiedOwner = false;

/**
 * One owner's row: a live owner bound in a context of its own, a step that moves its state on,
 * its clear or close and its binding anew (each false when the owner has neither), and the
 * resources its digest names, registering the owner asked about first. A row may hash its
 * owner itself through liveDigest and savedDigest when the owner has no appendNativeDigest.
 */
template <class Live>
concept LocalOwnerLive =
    std::default_initializable<Live> && requires(Live& live, const typename Live::Owner& owner) {
        { live.owner() } -> std::same_as<typename Live::Owner&>;
        live.advance();
        { live.clear() } -> std::same_as<bool>;
        { live.rebind() } -> std::same_as<bool>;
        { live.resources(owner) } -> std::same_as<NativeStateDigest::Resources>;
        { Live::kName } -> std::convertible_to<std::string_view>;
        { Live::kRules } -> std::convertible_to<LocalOwnerRules>;
    };

namespace local_owner {

using Digest = NativeStateDigest::Digest;

/** What asking an owner to hash a snapshot gave: whether it took it, and the digest after. */
struct PastDigest {
    bool appended = false;
    std::optional<Digest> digest;
};

/** Saves `owner` into `saved`; a save that can fail must not fail for a row's live owner. */
template <class Owner> void save(const Owner& owner, typename Owner::LocalSnapshot& saved) {
    if constexpr (std::is_same_v<decltype(owner.saveLocal(saved)), bool>) {
        REQUIRE(owner.saveLocal(saved));
    } else {
        owner.saveLocal(saved);
    }
}

/** `owner`'s live digest over the resources `live` names, or none when it cannot be hashed. */
template <class Live> std::optional<Digest> liveDigest(Live& live, typename Live::Owner& owner) {
    if constexpr (requires { live.liveDigest(owner); }) {
        return live.liveDigest(owner);
    } else {
        NativeStateDigest digest{live.resources(owner)};
        if (!owner.appendNativeDigest(digest)) {
            return std::nullopt;
        }
        return digest.checkedFinish();
    }
}

/** What `owner` gave when asked to hash `saved` over the resources `live` names. */
template <class Live>
PastDigest savedDigest(Live& live, typename Live::Owner& owner,
                       const typename Live::Owner::LocalSnapshot& saved) {
    if constexpr (requires { live.savedDigest(owner, saved); }) {
        const std::optional<Digest> digest = live.savedDigest(owner, saved);
        return {digest.has_value(), digest};
    } else {
        NativeStateDigest digest{live.resources(owner)};
        const bool appended = owner.appendLocalDigest(digest, saved);
        return {appended, digest.checkedFinish()};
    }
}

/** How `owner` answers `saved`. Whatever the answer, its three entry points must agree: an
 * admitted snapshot restores the live digest it hashed to, and a refused restore leaves the
 * owner's live digest as it was. */
template <class Live>
Admission ask(Live& live, typename Live::Owner& owner,
              const typename Live::Owner::LocalSnapshot& saved) {
    const bool admitted = owner.canRestoreLocal(saved);
    const PastDigest past = savedDigest(live, owner, saved);
    CHECK(past.appended == admitted);
    if (admitted) {
        CHECK(past.digest.has_value());
        CHECK(owner.restoreLocal(saved));
        const bool restored = liveDigest(live, owner) == past.digest;
        CHECK(restored);
        return Admission::Admitted;
    }
    const std::optional<Digest> before = liveDigest(live, owner);
    CHECK_FALSE(owner.restoreLocal(saved));
    const bool unchanged = liveDigest(live, owner) == before;
    CHECK(unchanged);
    return past.digest.has_value() ? Admission::RefusedKeepingDigest : Admission::Refused;
}

/** How a moved-from owner answers `saved`. Its values are unspecified, so it is asked only
 * whether it takes the snapshot, and a refusal must hold for its restore as well. */
template <class Owner>
Admission askMovedFrom(Owner& owner, const typename Owner::LocalSnapshot& saved) {
    if (owner.canRestoreLocal(saved)) {
        return Admission::Admitted;
    }
    CHECK_FALSE(owner.restoreLocal(saved));
    return Admission::Refused;
}

/** A live owner advanced once and what it saved then. */
template <class Live> struct Saved {
    std::unique_ptr<Live> live = std::make_unique<Live>();
    typename Live::Owner::LocalSnapshot snapshot;
    Saved() {
        live->advance();
        save(live->owner(), snapshot);
    }
};

} // namespace local_owner

/** Runs every question of `Live`'s row on fresh owners, each built by the row. */
template <LocalOwnerLive Live> LocalOwnerRules observeLocalOwner() {
    using Owner = typename Live::Owner;
    using Snapshot = typename Owner::LocalSnapshot;
    using local_owner::ask;
    using local_owner::Saved;
    LocalOwnerRules rules;
    {
        const Saved<Live> saved;
        auto other = std::make_unique<Live>();
        rules.foreign = ask(*other, other->owner(), saved.snapshot);
    }
    {
        auto live = std::make_unique<Live>();
        live->advance();
        const Snapshot blank{};
        rules.blank = ask(*live, live->owner(), blank);
    }
    {
        Saved<Live> saved;
        if (saved.live->clear()) {
            rules.cleared = ask(*saved.live, saved.live->owner(), saved.snapshot);
        }
    }
    {
        Saved<Live> saved;
        const bool restored = saved.live->owner().restoreLocal(saved.snapshot);
        CHECK(restored);
        if (restored && saved.live->clear()) {
            rules.restoredThenCleared = ask(*saved.live, saved.live->owner(), saved.snapshot);
        }
    }
    {
        Saved<Live> saved;
        if (saved.live->rebind()) {
            rules.rebound = ask(*saved.live, saved.live->owner(), saved.snapshot);
        }
    }
    if constexpr (std::is_move_constructible_v<Owner>) {
        Saved<Live> saved;
        auto moved = std::make_unique<Owner>(std::move(saved.live->owner()));
        rules.movedInto = ask(*saved.live, *moved, saved.snapshot);
        rules.movedFrom = local_owner::askMovedFrom(saved.live->owner(), saved.snapshot);
    }
    if constexpr (std::is_move_assignable_v<Owner>) {
        Saved<Live> saved;
        auto other = std::make_unique<Live>();
        other->owner() = std::move(saved.live->owner());
        // The assigned values still borrow the first context, so its resources name them.
        rules.assignedInto = ask(*saved.live, other->owner(), saved.snapshot);
    }
    if constexpr (kCopiedOwner<Owner>) {
        Saved<Live> saved;
        auto copy = std::make_unique<Owner>(std::as_const(saved.live->owner()));
        rules.copied = ask(*saved.live, *copy, saved.snapshot);
    }
    {
        Saved<Live> saved;
        Live& live = *saved.live;
        const auto before = local_owner::liveDigest(live, live.owner());
        REQUIRE(before.has_value());
        const auto past = local_owner::savedDigest(live, live.owner(), saved.snapshot).digest;
        live.advance();
        const auto after = local_owner::liveDigest(live, live.owner());
        const bool restored = live.owner().restoreLocal(saved.snapshot);
        rules.advances = after.has_value() && after != before;
        rules.roundTrips =
            restored && past == before && local_owner::liveDigest(live, live.owner()) == before;
    }
    return rules;
}

/** Checks that `Live`'s owner still answers as its row recorded. */
template <LocalOwnerLive Live> void checkLocalOwner() {
    CAPTURE(Live::kName);
    // Observed outside the assertion, so that a row skipped without the game data skips.
    const LocalOwnerRules observed = observeLocalOwner<Live>();
    CHECK(observed == Live::kRules);
}

/** How `Live`'s owner answers a snapshot saved before `act` was done to its row, for an owner
 * opened or bound in more ways than its row's rebind takes. */
template <LocalOwnerLive Live, std::invocable<Live&> Act> Admission askAfter(Act act) {
    CAPTURE(Live::kName);
    local_owner::Saved<Live> saved;
    std::invoke(act, *saved.live);
    return local_owner::ask(*saved.live, saved.live->owner(), saved.snapshot);
}

/** A default owner after a clear: its saved digest is its live one, another owner refuses
 * its snapshot, and a second clear refuses what was saved before it, spoiling the digest
 * each time. */
template <class Owner> void checkNativeOwnerGuards() {
    Owner owner;
    Owner foreign;
    owner.clear();
    foreign.clear();
    typename Owner::LocalSnapshot saved;
    owner.saveLocal(saved);
    NativeStateDigest live;
    NativeStateDigest past;
    REQUIRE(owner.appendNativeDigest(live));
    REQUIRE(owner.appendLocalDigest(past, saved));
    CHECK(live.checkedFinish() == past.checkedFinish());
    NativeStateDigest wrong;
    CHECK_FALSE(foreign.appendLocalDigest(wrong, saved));
    CHECK_FALSE(wrong.checkedFinish());
    owner.clear();
    NativeStateDigest stale;
    CHECK_FALSE(owner.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
}

} // namespace gdl::test
