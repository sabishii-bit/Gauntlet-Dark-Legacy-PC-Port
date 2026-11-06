#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/LocalBinding.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/LocalState.h"
#include "engine/io/NativeStateDigest.h"

namespace {
using namespace gdl;

using Digest = NativeStateDigest::Digest;

/** A count: the values of every synthetic owner below. */
struct Count {
    s32 value = 0;
    bool operator==(const Count&) const = default;
};

/** The live digest of `owner`, or none when it cannot be written. */
template <class Owner> std::optional<Digest> liveDigest(const Owner& owner) {
    NativeStateDigest digest;
    if (!owner.appendNativeDigest(digest)) {
        return std::nullopt;
    }
    return digest.checkedFinish();
}

/** The digest `owner` writes for `saved`, or none when it refuses it. */
template <class Owner>
std::optional<Digest> savedDigest(const Owner& owner, const typename Owner::LocalSnapshot& saved) {
    NativeStateDigest digest;
    if (!owner.appendLocalDigest(digest, saved)) {
        return std::nullopt;
    }
    return digest.checkedFinish();
}

/** A leaf of one count under a schema tag: a refusal spoils the digest, and no other trait is
 * on. */
class Leaf : public LocalState<Leaf> {
public:
    s32 count() const { return m_values.value; }
    void setCount(s32 value) { m_values.value = value; }
    /** Refuses what it saved before, as an owner does when it clears. */
    void rebind() { m_localBinding.rebind(); }

private:
    friend class LocalState<Leaf>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Count;
    struct LocalChildren {};
    static constexpr auto localParts() {
        return local::parts(local::tag("Leaf.native.1"), local::own());
    }
    const LocalValues& localValues() const { return m_values; }
    LocalValues& localValues() { return m_values; }
    bool digestLocalValues(NativeStateDigest& digest, const LocalValues& values) const {
        // Every member is the leaf's own, so the leaf destructures: the base adds none.
        [[maybe_unused]] const auto& [own, binding] = *this;
        digest.integer(values.value);
        return digest.good();
    }

    Count m_values;
    LocalBinding m_localBinding;
};

/** A count after a leaf and a leaf that may be absent: its tag first, then its children, its own
 * count last. */
class Composite : public LocalState<Composite> {
public:
    Leaf& first() { return m_first; }
    /** The leaf that may be absent, or none. */
    Leaf* extra() { return m_extra.get(); }
    void addExtra(s32 value) {
        m_extra = std::make_unique<Leaf>();
        m_extra->setCount(value);
    }
    void dropExtra() { m_extra.reset(); }
    s32 count() const { return m_values.value; }
    void setCount(s32 value) { m_values.value = value; }

private:
    friend class LocalState<Composite>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Count;
    struct LocalChildren {
        Leaf::LocalSnapshot first;
        std::optional<Leaf::LocalSnapshot> extra;
    };
    static constexpr auto localParts() {
        return local::parts(local::tag("Composite.native.1"),
                            local::child(&Composite::m_first, &LocalChildren::first),
                            local::optional(&Composite::m_extra, &LocalChildren::extra),
                            local::own());
    }
    const LocalValues& localValues() const { return m_values; }
    LocalValues& localValues() { return m_values; }
    static bool digestLocalValues(NativeStateDigest& digest, const LocalValues& values) {
        digest.integer(values.value);
        return digest.good();
    }

    Leaf m_first;
    std::unique_ptr<Leaf> m_extra;
    Count m_values;
    LocalBinding m_localBinding;
};

/** Which of its traits a probe turns on. */
enum class Trait : u8 { None, KeepsDigest, GatesOnBound, GatesOnValues, NotifiesRestore };

/** A leaf of one count that defines every hook and turns on the trait `On` names: a hook whose
 * trait is off is never called. */
template <Trait On> class Probe : public LocalState<Probe<On>> {
public:
    bool bound = true;     ///< what localBound() answers
    s32 mostAdmitted = 9;  ///< the largest count admitsLocalValues() accepts
    s32 restores = 0;      ///< how many times restoredLocal() was called
    s32 restoredCount = 0; ///< the count restoredLocal() last saw

    void setCount(s32 value) { m_values.value = value; }

private:
    friend class LocalState<Probe<On>>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = On != Trait::KeepsDigest,
                                              .gatesOnBound = On == Trait::GatesOnBound,
                                              .gatesOnValues = On == Trait::GatesOnValues,
                                              .notifiesRestore = On == Trait::NotifiesRestore};
    using LocalValues = Count;
    struct LocalChildren {};
    static constexpr auto localParts() { return local::parts(local::own()); }
    const LocalValues& localValues() const { return m_values; }
    LocalValues& localValues() { return m_values; }
    static bool digestLocalValues(NativeStateDigest& digest, const LocalValues& values) {
        digest.integer(values.value);
        return digest.good();
    }
    bool localBound() const { return bound; }
    bool admitsLocalValues(const LocalValues& values) const { return values.value <= mostAdmitted; }
    void restoredLocal() {
        ++restores;
        restoredCount = m_values.value;
    }

    Count m_values;
    LocalBinding m_localBinding;
};

/** A count after a child whose save fails while the child is not bound. */
class Holder : public LocalState<Holder> {
public:
    Probe<Trait::GatesOnBound>& child() { return m_child; }

private:
    friend class LocalState<Holder>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Count;
    struct LocalChildren {
        Probe<Trait::GatesOnBound>::LocalSnapshot child;
    };
    static constexpr auto localParts() {
        return local::parts(local::child(&Holder::m_child, &LocalChildren::child), local::own());
    }
    const LocalValues& localValues() const { return m_values; }
    LocalValues& localValues() { return m_values; }
    static bool digestLocalValues(NativeStateDigest& digest, const LocalValues& values) {
        digest.integer(values.value);
        return digest.good();
    }

    Probe<Trait::GatesOnBound> m_child;
    Count m_values;
    LocalBinding m_localBinding;
};

/** What the noted children and their parent were asked, in order: "save first" and so on. */
using Asked = std::vector<std::string>;

/** A child that notes what it is asked: an owner written by hand, beside the base's. */
class Noted {
public:
    /** The value a noted child saved, and which child saved it. */
    class LocalSnapshot {
        friend class Noted;
        const Noted* m_owner = nullptr;
        s32 m_value = 0;
    };

    Noted(std::string_view name, Asked& asked) : m_name(name), m_asked(&asked) {}

    void saveLocal(LocalSnapshot& saved) const {
        note("save");
        saved.m_owner = this;
        saved.m_value = m_value;
    }
    bool canRestoreLocal(const LocalSnapshot& saved) const {
        note("admit");
        return saved.m_owner == this;
    }
    bool restoreLocal(const LocalSnapshot& saved) {
        if (saved.m_owner != this) {
            return false;
        }
        note("restore");
        m_value = saved.m_value;
        return true;
    }
    bool appendNativeDigest(NativeStateDigest& digest) const {
        note("append");
        digest.integer(m_value);
        return digest.good();
    }
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
        if (saved.m_owner != this) {
            digest.invalidate();
            return false;
        }
        note("append");
        digest.integer(saved.m_value);
        return digest.good();
    }

private:
    void note(std::string_view what) const {
        m_asked->push_back(std::format("{} {}", what, m_name));
    }

    std::string_view m_name;
    Asked* m_asked;
    s32 m_value = 0;
};

/** A parent whose own values stand between two noted children, noting each time its values are
 * read, written, admitted or hashed. */
class Ordered : public LocalState<Ordered> {
public:
    explicit Ordered(Asked& asked)
        : m_first("first", asked), m_second("second", asked), m_asked(&asked) {}

private:
    friend class LocalState<Ordered>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = true,
                                              .notifiesRestore = false};
    using LocalValues = Count;
    struct LocalChildren {
        Noted::LocalSnapshot first;
        Noted::LocalSnapshot second;
    };
    static constexpr auto localParts() {
        return local::parts(local::child(&Ordered::m_first, &LocalChildren::first), local::own(),
                            local::child(&Ordered::m_second, &LocalChildren::second));
    }
    const LocalValues& localValues() const {
        m_asked->emplace_back("read own");
        return m_values;
    }
    LocalValues& localValues() {
        m_asked->emplace_back("write own");
        return m_values;
    }
    bool admitsLocalValues(const LocalValues& /*values*/) const {
        m_asked->emplace_back("admit own");
        return true;
    }
    bool digestLocalValues(NativeStateDigest& digest, const LocalValues& values) const {
        m_asked->emplace_back("append own");
        digest.integer(values.value);
        return digest.good();
    }

    Noted m_first;
    Noted m_second;
    Count m_values;
    LocalBinding m_localBinding;
    Asked* m_asked;
};

TEST_CASE("an owner on the base is one a roster holds, and the base holds nothing",
          "[local-state]") {
    STATIC_REQUIRE(LocalOwner<Leaf>);
    STATIC_REQUIRE(LocalOwner<Composite>);
    STATIC_REQUIRE(LocalOwner<Holder>);
    STATIC_REQUIRE(LocalOwner<Ordered>);
    STATIC_REQUIRE(LocalOwner<Probe<Trait::None>>);
    STATIC_REQUIRE(std::is_empty_v<LocalState<Leaf>>);
    STATIC_REQUIRE(std::is_copy_constructible_v<Leaf::LocalSnapshot>);
    // Every trait is stated: an answer has no default to fall back on.
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<LocalTraits::Answer>);
    STATIC_REQUIRE(std::is_convertible_v<bool, LocalTraits::Answer>);
}

TEST_CASE("a leaf saves, restores and hashes its values under its binding", "[local-state]") {
    Leaf leaf;
    leaf.setCount(3);
    Leaf::LocalSnapshot saved;
    REQUIRE(leaf.saveLocal(saved));
    CHECK(leaf.canRestoreLocal(saved));
    const auto before = liveDigest(leaf);
    REQUIRE(before.has_value());
    NativeStateDigest expected;
    expected.string("Leaf.native.1");
    expected.integer(3);
    CHECK(before == expected.checkedFinish());
    CHECK(savedDigest(leaf, saved) == before);
    leaf.setCount(5);
    CHECK(liveDigest(leaf) != before);
    CHECK(savedDigest(leaf, saved) == before);
    REQUIRE(leaf.restoreLocal(saved));
    CHECK(leaf.count() == 3);
    CHECK(liveDigest(leaf) == before);
}

TEST_CASE("a leaf compares only snapshots it admits", "[local-state]") {
    Leaf leaf;
    leaf.setCount(3);
    Leaf::LocalSnapshot first;
    Leaf::LocalSnapshot second;
    REQUIRE(leaf.saveLocal(first));
    REQUIRE(leaf.saveLocal(second));
    CHECK(leaf.equalLocal(first, second));
    leaf.setCount(4);
    Leaf::LocalSnapshot later;
    REQUIRE(leaf.saveLocal(later));
    CHECK_FALSE(leaf.equalLocal(first, later));
    // Another leaf's snapshot of the same count is not this leaf's to compare.
    Leaf other;
    other.setCount(3);
    Leaf::LocalSnapshot ofOther;
    REQUIRE(other.saveLocal(ofOther));
    CHECK_FALSE(leaf.equalLocal(first, ofOther));
}

TEST_CASE("a leaf refuses another's snapshot, a default one and one saved before a rebind or a "
          "move",
          "[local-state]") {
    Leaf leaf;
    leaf.setCount(3);
    const Leaf other;
    Leaf::LocalSnapshot ofOther;
    REQUIRE(other.saveLocal(ofOther));
    CHECK_FALSE(leaf.canRestoreLocal(ofOther));
    CHECK_FALSE(leaf.restoreLocal(ofOther));
    CHECK(leaf.count() == 3);
    NativeStateDigest refused;
    CHECK_FALSE(leaf.appendLocalDigest(refused, ofOther));
    CHECK_FALSE(refused.checkedFinish().has_value());
    CHECK_FALSE(leaf.canRestoreLocal(Leaf::LocalSnapshot{}));

    Leaf::LocalSnapshot beforeRebind;
    REQUIRE(leaf.saveLocal(beforeRebind));
    leaf.rebind();
    CHECK_FALSE(leaf.canRestoreLocal(beforeRebind));
    CHECK_FALSE(leaf.restoreLocal(beforeRebind));

    // A move rebinds both sides, and the values go with it.
    const auto source = std::make_unique<Leaf>();
    source->setCount(4);
    Leaf::LocalSnapshot beforeMove;
    REQUIRE(source->saveLocal(beforeMove));
    const Leaf moved{std::move(*source)};
    CHECK(moved.count() == 4);
    CHECK_FALSE(moved.canRestoreLocal(beforeMove));
    CHECK_FALSE(source->canRestoreLocal(beforeMove));
}

TEST_CASE("a roster writes its tag first, each child where it stands and the owner's values last",
          "[local-state]") {
    Composite composite;
    composite.first().setCount(1);
    composite.addExtra(2);
    composite.setCount(3);
    NativeStateDigest expected;
    expected.string("Composite.native.1");
    expected.string("Leaf.native.1");
    expected.integer(1);
    expected.flag(true);
    expected.string("Leaf.native.1");
    expected.integer(2);
    expected.integer(3);
    CHECK(liveDigest(composite) == expected.checkedFinish());
}

TEST_CASE("a composite saves, compares and restores its children with its own values",
          "[local-state]") {
    Composite composite;
    composite.first().setCount(1);
    composite.addExtra(2);
    composite.setCount(3);
    Composite::LocalSnapshot saved;
    REQUIRE(composite.saveLocal(saved));
    Composite::LocalSnapshot again;
    REQUIRE(composite.saveLocal(again));
    CHECK(composite.equalLocal(saved, again));
    const auto before = liveDigest(composite);
    REQUIRE(before.has_value());
    composite.first().setCount(4);
    composite.extra()->setCount(5);
    composite.setCount(6);
    Composite::LocalSnapshot later;
    REQUIRE(composite.saveLocal(later));
    CHECK_FALSE(composite.equalLocal(saved, later));
    CHECK(liveDigest(composite) != before);
    CHECK(savedDigest(composite, saved) == before);
    REQUIRE(composite.restoreLocal(saved));
    CHECK(composite.first().count() == 1);
    CHECK(composite.extra()->count() == 2);
    CHECK(composite.count() == 3);
    CHECK(liveDigest(composite) == before);
    // A child that refuses its part refuses the whole snapshot.
    composite.first().rebind();
    CHECK_FALSE(composite.canRestoreLocal(saved));
    NativeStateDigest refused;
    CHECK_FALSE(composite.appendLocalDigest(refused, saved));
    CHECK_FALSE(refused.checkedFinish().has_value());
}

TEST_CASE("an optional child is saved with its presence and refused with the other one",
          "[local-state]") {
    Composite composite;
    Composite::LocalSnapshot withoutExtra;
    REQUIRE(composite.saveLocal(withoutExtra));
    NativeStateDigest absent;
    absent.string("Composite.native.1");
    absent.string("Leaf.native.1");
    absent.integer(0);
    absent.flag(false);
    absent.integer(0);
    CHECK(savedDigest(composite, withoutExtra) == absent.checkedFinish());
    composite.addExtra(2);
    CHECK_FALSE(composite.canRestoreLocal(withoutExtra));
    NativeStateDigest stale;
    CHECK_FALSE(composite.appendLocalDigest(stale, withoutExtra));
    CHECK_FALSE(stale.checkedFinish().has_value());
    Composite::LocalSnapshot withExtra;
    REQUIRE(composite.saveLocal(withExtra));
    CHECK(composite.canRestoreLocal(withExtra));
    composite.dropExtra();
    CHECK_FALSE(composite.canRestoreLocal(withExtra));
    CHECK(composite.canRestoreLocal(withoutExtra));
}

TEST_CASE("a child that cannot save leaves its parent's snapshot unstamped", "[local-state]") {
    Holder holder;
    Holder::LocalSnapshot saved;
    REQUIRE(holder.saveLocal(saved));
    REQUIRE(holder.canRestoreLocal(saved));
    holder.child().bound = false;
    CHECK_FALSE(holder.saveLocal(saved));
    holder.child().bound = true;
    // What it admitted before the failed save, it admits no more.
    CHECK_FALSE(holder.canRestoreLocal(saved));
    CHECK_FALSE(holder.restoreLocal(saved));
    NativeStateDigest refused;
    CHECK_FALSE(holder.appendLocalDigest(refused, saved));
    CHECK_FALSE(refused.checkedFinish().has_value());
    REQUIRE(holder.saveLocal(saved));
    CHECK(holder.canRestoreLocal(saved));
}

TEST_CASE("every operation reaches the owner's values where its roster names them",
          "[local-state]") {
    Asked asked;
    Ordered ordered{asked};
    Ordered::LocalSnapshot saved;
    REQUIRE(ordered.saveLocal(saved));
    CHECK(asked == Asked{"save first", "read own", "save second"});
    asked.clear();
    CHECK(ordered.canRestoreLocal(saved));
    CHECK(asked == Asked{"admit first", "admit own", "admit second"});
    asked.clear();
    NativeStateDigest live;
    CHECK(ordered.appendNativeDigest(live));
    CHECK(asked == Asked{"append first", "read own", "append own", "append second"});
    asked.clear();
    NativeStateDigest past;
    CHECK(ordered.appendLocalDigest(past, saved));
    CHECK(asked == Asked{"admit first", "admit own", "admit second", "append first", "append own",
                         "append second"});
    asked.clear();
    CHECK(ordered.restoreLocal(saved));
    CHECK(asked == Asked{"admit first", "admit own", "admit second", "restore first", "write own",
                         "restore second"});
}

TEST_CASE("a refused snapshot spoils the digest, unless the owner keeps it good", "[local-state]") {
    using Keeper = Probe<Trait::KeepsDigest>;
    const Keeper keeper;
    const Keeper stranger;
    Keeper::LocalSnapshot ofStranger;
    REQUIRE(stranger.saveLocal(ofStranger));
    CHECK_FALSE(keeper.canRestoreLocal(ofStranger));
    NativeStateDigest kept;
    CHECK_FALSE(keeper.appendLocalDigest(kept, ofStranger));
    // Nothing was written, and the digest is still good.
    CHECK(kept.checkedFinish() == NativeStateDigest{}.checkedFinish());

    using Spoiler = Probe<Trait::None>;
    const Spoiler spoiler;
    const Spoiler other;
    Spoiler::LocalSnapshot ofOther;
    REQUIRE(other.saveLocal(ofOther));
    NativeStateDigest spoilt;
    CHECK_FALSE(spoiler.appendLocalDigest(spoilt, ofOther));
    CHECK_FALSE(spoilt.checkedFinish().has_value());
}

TEST_CASE("an owner that gates on its binding saves and admits only while bound, and hashes "
          "itself either way",
          "[local-state]") {
    using Gated = Probe<Trait::GatesOnBound>;
    Gated gated;
    gated.setCount(3);
    Gated::LocalSnapshot saved;
    REQUIRE(gated.saveLocal(saved));
    Gated::LocalSnapshot overwritten;
    REQUIRE(gated.saveLocal(overwritten));
    gated.bound = false;
    CHECK_FALSE(gated.canRestoreLocal(saved));
    CHECK_FALSE(gated.restoreLocal(saved));
    NativeStateDigest refused;
    CHECK_FALSE(gated.appendLocalDigest(refused, saved));
    CHECK_FALSE(refused.checkedFinish().has_value());
    // Its save fails and unstamps the snapshot it was handed...
    CHECK_FALSE(gated.saveLocal(overwritten));
    // ...yet its live digest is written all the same.
    NativeStateDigest live;
    CHECK(gated.appendNativeDigest(live));
    CHECK(live.checkedFinish().has_value());
    gated.bound = true;
    CHECK(gated.canRestoreLocal(saved));
    CHECK_FALSE(gated.canRestoreLocal(overwritten));

    // With the trait off, the hook is never asked.
    using Ungated = Probe<Trait::None>;
    Ungated ungated;
    ungated.bound = false;
    Ungated::LocalSnapshot unbound;
    CHECK(ungated.saveLocal(unbound));
    CHECK(ungated.canRestoreLocal(unbound));
}

TEST_CASE("an owner that gates on its values admits only the values it accepts", "[local-state]") {
    using Gated = Probe<Trait::GatesOnValues>;
    Gated gated;
    gated.setCount(gated.mostAdmitted + 1);
    Gated::LocalSnapshot tooMany;
    REQUIRE(gated.saveLocal(tooMany));
    CHECK_FALSE(gated.canRestoreLocal(tooMany));
    CHECK_FALSE(gated.restoreLocal(tooMany));
    NativeStateDigest refused;
    CHECK_FALSE(gated.appendLocalDigest(refused, tooMany));
    CHECK_FALSE(refused.checkedFinish().has_value());
    gated.setCount(gated.mostAdmitted);
    Gated::LocalSnapshot enough;
    REQUIRE(gated.saveLocal(enough));
    CHECK(gated.canRestoreLocal(enough));

    using Ungated = Probe<Trait::None>;
    Ungated ungated;
    ungated.setCount(ungated.mostAdmitted + 1);
    Ungated::LocalSnapshot any;
    REQUIRE(ungated.saveLocal(any));
    CHECK(ungated.canRestoreLocal(any));
}

TEST_CASE("an owner that asks to hear of restores hears of each one after it is taken",
          "[local-state]") {
    using Told = Probe<Trait::NotifiesRestore>;
    Told told;
    told.setCount(3);
    Told::LocalSnapshot saved;
    REQUIRE(told.saveLocal(saved));
    told.setCount(4);
    REQUIRE(told.restoreLocal(saved));
    CHECK(told.restores == 1);
    CHECK(told.restoredCount == 3);
    REQUIRE(told.restoreLocal(saved));
    CHECK(told.restores == 2);
    // A refused restore is not heard of.
    const Told other;
    Told::LocalSnapshot ofOther;
    REQUIRE(other.saveLocal(ofOther));
    CHECK_FALSE(told.restoreLocal(ofOther));
    CHECK(told.restores == 2);

    using Untold = Probe<Trait::None>;
    Untold untold;
    Untold::LocalSnapshot quiet;
    REQUIRE(untold.saveLocal(quiet));
    REQUIRE(untold.restoreLocal(quiet));
    CHECK(untold.restores == 0);
}

} // namespace
