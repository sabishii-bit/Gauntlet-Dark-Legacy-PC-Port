#pragma once

#include <concepts>
#include <functional>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "engine/io/NativeStateDigest.h"

namespace gdl {

/** What a local rollback history asks of an owner: a snapshot only it fills and admits, a restore
 * that cannot fail once admitted, and one digest stream for its live values and any snapshot it
 * admits. Governs owners compiled into the game; a mechanic's state crosses GameplayMechanic. */
template <class T>
concept LocalOwner = requires(const T& owner, T& live, typename T::LocalSnapshot& out,
                              const typename T::LocalSnapshot& in, NativeStateDigest& digest) {
    owner.saveLocal(out);
    { owner.canRestoreLocal(in) } -> std::same_as<bool>;
    { live.restoreLocal(in) } -> std::same_as<bool>;
    { owner.appendNativeDigest(digest) } -> std::same_as<bool>;
    { owner.appendLocalDigest(digest, in) } -> std::same_as<bool>;
};

/**
 * An owner's roster: the parts of its snapshot in digest order, which saving, admitting,
 * restoring, comparing and hashing all walk in that one order. A part is the owner's own values,
 * a schema tag, a child owner with its slot in the snapshot, an optional child, or a sample: a
 * part the owner measures under a profile name. Each fold takes the owner's own-values step,
 * which copies, admits, takes back, compares or writes those values as its fold asks; a roster
 * that names no own() passes noValues().
 */
namespace local {

/** The owner's own values, at this place in its roster. */
struct Own {};
constexpr Own own() {
    return {};
}
/** The own-values step of a roster that names no own(); a roster that names one refuses it. */
struct NoValues {};
constexpr NoValues noValues() {
    return {};
}
/** A schema string the digest writes at this place. */
struct Tag {
    std::string_view text;
};
constexpr Tag tag(std::string_view text) {
    return {text};
}
/** A child owner reached from the parent (member pointer or captureless lambda) and its slot. */
template <class Reach, class Slot> struct Child {
    Reach reach;
    Slot slot;
};
/** A child that may be absent, reached as a pointer, its slot an optional snapshot: its presence
 * flag, then the child. A snapshot is admitted only when its presence matches the live owner's. */
template <class Reach, class Slot> struct Optional {
    Reach reach;
    Slot slot;
};
/** A part the scene measures under a profile name. */
template <class Part> struct Sample {
    std::string_view name;
    Part part;
};
constexpr auto child(auto reach, auto slot) {
    return Child<decltype(reach), decltype(slot)>{reach, slot};
}
constexpr auto optional(auto reach, auto slot) {
    return Optional<decltype(reach), decltype(slot)>{reach, slot};
}
constexpr auto sample(std::string_view name, auto part) {
    return Sample<decltype(part)>{name, part};
}
template <class... Part> constexpr std::tuple<Part...> parts(Part... part) {
    return {part...};
}

/** Saves `owner` into `slot`; an owner whose save returns nothing always succeeds. */
template <LocalOwner Owner>
bool saveOwner(const Owner& owner, typename Owner::LocalSnapshot& slot) {
    if constexpr (std::is_void_v<decltype(owner.saveLocal(slot))>) {
        owner.saveLocal(slot);
        return true;
    } else {
        return owner.saveLocal(slot);
    }
}

// One part of a roster, for each fold below. A sample is its part.

template <class O, class S, class F>
bool savePart(const O& /*owner*/, const Own& /*part*/, S& /*slots*/, const F& values) {
    static_assert(!std::is_same_v<F, NoValues>, "A roster that names own needs its values step");
    if constexpr (std::is_void_v<std::invoke_result_t<const F&>>) {
        values();
        return true;
    } else {
        return values();
    }
}
template <class O, class S, class F>
bool savePart(const O& /*owner*/, const Tag& /*part*/, S& /*slots*/, const F& /*values*/) {
    return true;
}
template <class O, class Reach, class Slot, class S, class F>
bool savePart(const O& owner, const Child<Reach, Slot>& part, S& slots, const F& /*values*/) {
    return local::saveOwner(std::invoke(part.reach, owner), std::invoke(part.slot, slots));
}
template <class O, class Reach, class Slot, class S, class F>
bool savePart(const O& owner, const Optional<Reach, Slot>& part, S& slots, const F& /*values*/) {
    const auto& reached = std::invoke(part.reach, owner);
    auto& slot = std::invoke(part.slot, slots);
    if (reached == nullptr) {
        slot.reset();
        return true;
    }
    if (!slot.has_value()) {
        slot.emplace();
    }
    return local::saveOwner(*reached, *slot);
}
template <class O, class Part, class S, class F>
bool savePart(const O& owner, const Sample<Part>& part, S& slots, const F& values) {
    return local::savePart(owner, part.part, slots, values);
}

template <class O, class S, class F>
bool admitsPart(const O& /*owner*/, const Own& /*part*/, const S& /*slots*/, const F& values) {
    static_assert(!std::is_same_v<F, NoValues>, "A roster that names own needs its values step");
    return values();
}
template <class O, class S, class F>
bool admitsPart(const O& /*owner*/, const Tag& /*part*/, const S& /*slots*/, const F& /*values*/) {
    return true;
}
template <class O, class Reach, class Slot, class S, class F>
bool admitsPart(const O& owner, const Child<Reach, Slot>& part, const S& slots,
                const F& /*values*/) {
    return std::invoke(part.reach, owner).canRestoreLocal(std::invoke(part.slot, slots));
}
template <class O, class Reach, class Slot, class S, class F>
bool admitsPart(const O& owner, const Optional<Reach, Slot>& part, const S& slots,
                const F& /*values*/) {
    const auto& reached = std::invoke(part.reach, owner);
    const auto& slot = std::invoke(part.slot, slots);
    if (slot.has_value() != (reached != nullptr)) {
        return false;
    }
    return !slot.has_value() || reached->canRestoreLocal(*slot);
}
template <class O, class Part, class S, class F>
bool admitsPart(const O& owner, const Sample<Part>& part, const S& slots, const F& values) {
    return local::admitsPart(owner, part.part, slots, values);
}

template <class O, class S, class F>
void restorePart(O& /*owner*/, const Own& /*part*/, const S& /*slots*/, const F& values) {
    static_assert(!std::is_same_v<F, NoValues>, "A roster that names own needs its values step");
    values();
}
template <class O, class S, class F>
void restorePart(O& /*owner*/, const Tag& /*part*/, const S& /*slots*/, const F& /*values*/) {}
template <class O, class Reach, class Slot, class S, class F>
void restorePart(O& owner, const Child<Reach, Slot>& part, const S& slots, const F& /*values*/) {
    // Admitted with the whole snapshot beforehand, so the child takes it.
    std::invoke(part.reach, owner).restoreLocal(std::invoke(part.slot, slots));
}
template <class O, class Reach, class Slot, class S, class F>
void restorePart(O& owner, const Optional<Reach, Slot>& part, const S& slots, const F& /*values*/) {
    const auto& reached = std::invoke(part.reach, owner);
    const auto& slot = std::invoke(part.slot, slots);
    if (reached != nullptr && slot.has_value()) {
        reached->restoreLocal(*slot);
    }
}
template <class O, class Part, class S, class F>
void restorePart(O& owner, const Sample<Part>& part, const S& slots, const F& values) {
    local::restorePart(owner, part.part, slots, values);
}

template <class O, class S, class F>
bool equalPart(const O& /*owner*/, const Own& /*part*/, const S& /*left*/, const S& /*right*/,
               const F& values) {
    static_assert(!std::is_same_v<F, NoValues>, "A roster that names own needs its values step");
    return values();
}
template <class O, class S, class F>
bool equalPart(const O& /*owner*/, const Tag& /*part*/, const S& /*left*/, const S& /*right*/,
               const F& /*values*/) {
    return true;
}
template <class O, class Reach, class Slot, class S, class F>
bool equalPart(const O& owner, const Child<Reach, Slot>& part, const S& left, const S& right,
               const F& /*values*/) {
    return std::invoke(part.reach, owner)
        .equalLocal(std::invoke(part.slot, left), std::invoke(part.slot, right));
}
template <class O, class Reach, class Slot, class S, class F>
bool equalPart(const O& owner, const Optional<Reach, Slot>& part, const S& left, const S& right,
               const F& /*values*/) {
    const auto& reached = std::invoke(part.reach, owner);
    const auto& first = std::invoke(part.slot, left);
    const auto& second = std::invoke(part.slot, right);
    if (first.has_value() != second.has_value()) {
        return false;
    }
    return !first.has_value() || (reached != nullptr && reached->equalLocal(*first, *second));
}
template <class O, class Part, class S, class F>
bool equalPart(const O& owner, const Sample<Part>& part, const S& left, const S& right,
               const F& values) {
    return local::equalPart(owner, part.part, left, right, values);
}

template <class O, class S, class F>
bool appendPart(const O& /*owner*/, const Own& /*part*/, NativeStateDigest& /*digest*/,
                const S* /*saved*/, const F& values) {
    static_assert(!std::is_same_v<F, NoValues>, "A roster that names own needs its values step");
    return values();
}
template <class O, class S, class F>
bool appendPart(const O& /*owner*/, const Tag& part, NativeStateDigest& digest, const S* /*saved*/,
                const F& /*values*/) {
    digest.string(part.text);
    return true;
}
template <class O, class Reach, class Slot, class S, class F>
bool appendPart(const O& owner, const Child<Reach, Slot>& part, NativeStateDigest& digest,
                const S* saved, const F& /*values*/) {
    const auto& held = std::invoke(part.reach, owner);
    return saved != nullptr ? held.appendLocalDigest(digest, std::invoke(part.slot, *saved))
                            : held.appendNativeDigest(digest);
}
template <class O, class Reach, class Slot, class S, class F>
bool appendPart(const O& owner, const Optional<Reach, Slot>& part, NativeStateDigest& digest,
                const S* saved, const F& /*values*/) {
    const auto& reached = std::invoke(part.reach, owner);
    if (saved == nullptr) {
        digest.flag(reached != nullptr);
        return reached == nullptr || reached->appendNativeDigest(digest);
    }
    const auto& slot = std::invoke(part.slot, *saved);
    if (slot.has_value() != (reached != nullptr)) {
        digest.invalidate();
        return false;
    }
    digest.flag(slot.has_value());
    return !slot.has_value() || reached->appendLocalDigest(digest, *slot);
}
template <class O, class Part, class S, class F>
bool appendPart(const O& owner, const Sample<Part>& part, NativeStateDigest& digest, const S* saved,
                const F& values) {
    return local::appendPart(owner, part.part, digest, saved, values);
}

/** Saves each part in roster order, stopping at the first save that fails (a void save always
 * succeeds); `values` copies the owner's values. */
template <class O, class R, class S, class F>
bool save(const O& owner, const R& roster, S& slots, const F& values) {
    return std::apply(
        [&](const auto&... part) { return (local::savePart(owner, part, slots, values) && ...); },
        roster);
}
/** Whether every part admits its slot, in roster order, short-circuiting like &&. */
template <class O, class R, class S, class F>
bool admits(const O& owner, const R& roster, const S& slots, const F& values) {
    return std::apply(
        [&](const auto&... part) { return (local::admitsPart(owner, part, slots, values) && ...); },
        roster);
}
/** Restores every part in roster order after the caller has admitted the whole snapshot. */
template <class O, class R, class S, class F>
void restore(O& owner, const R& roster, const S& slots, const F& values) {
    std::apply([&](const auto&... part) { (local::restorePart(owner, part, slots, values), ...); },
               roster);
}
/** Whether two admitted snapshots hold equal parts, in roster order. */
template <class O, class R, class S, class F>
bool equal(const O& owner, const R& roster, const S& left, const S& right, const F& values) {
    return std::apply(
        [&](const auto&... part) {
            return (local::equalPart(owner, part, left, right, values) && ...);
        },
        roster);
}
/** Writes tags, own values (through `values`) and children, live or from `saved`, in roster
 * order; stops at the first part that fails. */
template <class O, class R, class S, class F>
bool append(const O& owner, const R& roster, NativeStateDigest& digest, const S* saved,
            const F& values) {
    const bool appended = std::apply(
        [&](const auto&... part) {
            return (local::appendPart(owner, part, digest, saved, values) && ...);
        },
        roster);
    return appended && digest.good();
}
/** Visits each sample's name and part in order, for an owner that measures and carries on. */
template <class R, class F> void eachSample(const R& roster, const F& visit) {
    std::apply([&](const auto&... each) { (visit(each.name, each.part), ...); }, roster);
}

} // namespace local
} // namespace gdl
