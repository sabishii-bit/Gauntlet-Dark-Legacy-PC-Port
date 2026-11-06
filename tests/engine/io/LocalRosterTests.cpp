#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/NativeStateDigest.h"

namespace {
using namespace gdl;

/** What the synthetic owners were asked, in order: "save first", "admit second" and so on. */
using Asked = std::vector<std::string>;

/** A leaf owner of one value. Its snapshot is admitted only by the leaf that saved it; it notes
 * what it is asked, and can be told to spoil its digest. */
class Leaf {
public:
    /** The value a leaf saved, and which leaf saved it. */
    class LocalSnapshot {
        friend class Leaf;
        const Leaf* m_owner = nullptr;
        s32 m_value = 0;
    };

    explicit Leaf(std::string_view name, Asked* asked = nullptr) : m_name(name), m_asked(asked) {}

    s32 value = 0;
    bool spoilsDigest = false; ///< its digest fails, as one naming an unbound resource does

    void saveLocal(LocalSnapshot& saved) const {
        note("save");
        saved.m_owner = this;
        saved.m_value = value;
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
        value = saved.m_value;
        return true;
    }
    bool appendNativeDigest(NativeStateDigest& digest) const { return appendValue(digest, value); }
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
        if (saved.m_owner != this) {
            digest.invalidate();
            return false;
        }
        return appendValue(digest, saved.m_value);
    }
    static bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) {
        return left.m_value == right.m_value;
    }

protected:
    void note(std::string_view what) const {
        if (m_asked != nullptr) {
            m_asked->push_back(std::format("{} {}", what, m_name));
        }
    }

private:
    bool appendValue(NativeStateDigest& digest, s32 written) const {
        note("append");
        if (spoilsDigest) {
            digest.invalidate();
            return false;
        }
        digest.integer(written);
        return digest.good();
    }

    std::string_view m_name;
    Asked* m_asked;
};

/** A leaf whose save can fail, as the opponents' does when they cannot fill a snapshot. */
class FallibleLeaf : public Leaf {
public:
    using Leaf::Leaf;

    bool failsSave = false;

    bool saveLocal(LocalSnapshot& saved) const {
        if (failsSave) {
            note("fail");
            return false;
        }
        Leaf::saveLocal(saved);
        return true;
    }
};

/** Saves, admits and restores, but writes no digest: no owner a roster holds. */
struct Undigested {
    struct LocalSnapshot {};
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
};

/** A parent of three leaves, the third's save fallible, and a count of its own. */
struct Parent {
    explicit Parent(Asked* asked = nullptr)
        : first("first", asked), second("second", asked), third("third", asked) {}

    Leaf first;
    Leaf second;
    FallibleLeaf third;
    s32 count = 0;
};

/** Where a parent's snapshot keeps each leaf's, and its count. */
struct Slots {
    Leaf::LocalSnapshot first;
    Leaf::LocalSnapshot second;
    Leaf::LocalSnapshot third;
    s32 count = 0;
};

/** A parent's tag, its first two leaves, then its count. */
constexpr auto kParentParts =
    local::parts(local::tag("Parent.native.1"), local::child(&Parent::first, &Slots::first),
                 local::child(&Parent::second, &Slots::second), local::own());

/** A parent of a leaf that may be absent. */
struct Holder {
    std::unique_ptr<Leaf> extra;
};

/** Where a holder's snapshot keeps its leaf's, when it had one. */
struct HolderSlots {
    std::optional<Leaf::LocalSnapshot> extra;
};

constexpr auto kHolderParts = local::parts(local::optional(&Holder::extra, &HolderSlots::extra));

/** Two owners reached through accessors and a plain value saved beside them: the shape of the
 * ceremonies, whose promotion and relics stand in the scene's roster beside their voices. */
class Ceremonies {
public:
    explicit Ceremonies(Asked* asked = nullptr)
        : m_promotion("promotion", asked), m_relics("relics", asked) {}

    Leaf& promotion() { return m_promotion; }
    const Leaf& promotion() const { return m_promotion; }
    Leaf& relics() { return m_relics; }
    const Leaf& relics() const { return m_relics; }

    f64 voice = 0; ///< saved and restored beside the roster, by assignment

private:
    Leaf m_promotion;
    Leaf m_relics;
};

/** Where the ceremonies' snapshot keeps their two owners' and their value. */
struct CeremonySlots {
    Leaf::LocalSnapshot promotion;
    Leaf::LocalSnapshot relics;
    f64 voice = 0;
};

constexpr auto kCeremonyParts = local::parts(
    local::sample("promotion", local::child([](auto& owner) -> auto& { return owner.promotion(); },
                                            &CeremonySlots::promotion)),
    local::sample("relics", local::child([](auto& owner) -> auto& { return owner.relics(); },
                                         &CeremonySlots::relics)));

/** A parent's digest through `parts`, live or from `saved`, its count written where they name
 * its own values; none when a part fails. */
template <class Parts>
std::optional<NativeStateDigest::Digest> digestOf(const Parent& parent, const Parts& parts,
                                                  const Slots* saved) {
    NativeStateDigest digest;
    const bool appended = local::append(parent, parts, digest, saved, [&] {
        digest.integer(saved != nullptr ? saved->count : parent.count);
        return digest.good();
    });
    return appended ? digest.checkedFinish() : std::nullopt;
}

TEST_CASE("an owner with the five entry points is one a roster holds", "[local-roster]") {
    STATIC_REQUIRE(LocalOwner<Leaf>);
    STATIC_REQUIRE(LocalOwner<FallibleLeaf>);
    STATIC_REQUIRE_FALSE(LocalOwner<Undigested>);
}

TEST_CASE("a leaf saves, admits, restores and hashes through its roster", "[local-roster]") {
    Parent parent;
    parent.first.value = 3;
    constexpr auto kParts = local::parts(local::child(&Parent::first, &Slots::first));
    Slots saved;
    REQUIRE(local::save(parent, kParts, saved, local::noValues()));
    CHECK(local::admits(parent, kParts, saved, local::noValues()));
    // The roster hashes the leaf as the leaf hashes itself.
    NativeStateDigest viaRoster;
    NativeStateDigest direct;
    CHECK(local::append(parent, kParts, viaRoster, &saved, local::noValues()));
    CHECK(parent.first.appendLocalDigest(direct, saved.first));
    CHECK(viaRoster.checkedFinish() == direct.checkedFinish());
    parent.first.value = 5;
    local::restore(parent, kParts, saved, local::noValues());
    CHECK(parent.first.value == 3);
    // Another parent's leaf refuses it.
    const Parent other;
    CHECK_FALSE(local::admits(other, kParts, saved, local::noValues()));
}

TEST_CASE("a composite saves, compares and restores each child and its own values",
          "[local-roster]") {
    Parent parent;
    parent.first.value = 1;
    parent.second.value = 2;
    parent.count = 3;
    Slots saved;
    REQUIRE(local::save(parent, kParentParts, saved, [&] { saved.count = parent.count; }));
    Slots again;
    REQUIRE(local::save(parent, kParentParts, again, [&] { again.count = parent.count; }));
    CHECK(local::equal(parent, kParentParts, saved, again,
                       [&] { return saved.count == again.count; }));
    parent.first.value = 4;
    parent.second.value = 5;
    parent.count = 6;
    Slots later;
    REQUIRE(local::save(parent, kParentParts, later, [&] { later.count = parent.count; }));
    CHECK_FALSE(local::equal(parent, kParentParts, saved, later,
                             [&] { return saved.count == later.count; }));
    REQUIRE(local::admits(parent, kParentParts, saved, [] { return true; }));
    local::restore(parent, kParentParts, saved, [&] { parent.count = saved.count; });
    CHECK(parent.first.value == 1);
    CHECK(parent.second.value == 2);
    CHECK(parent.count == 3);
}

TEST_CASE("an optional child writes its presence, then itself, and refuses the other presence",
          "[local-roster]") {
    Holder holder;
    HolderSlots absent;
    REQUIRE(local::save(holder, kHolderParts, absent, local::noValues()));
    CHECK_FALSE(absent.extra.has_value());
    CHECK(local::admits(holder, kHolderParts, absent, local::noValues()));
    NativeStateDigest live;
    CHECK(local::append(holder, kHolderParts, live, static_cast<const HolderSlots*>(nullptr),
                        local::noValues()));
    NativeStateDigest flagAlone;
    flagAlone.flag(false);
    CHECK(live.checkedFinish() == flagAlone.checkedFinish());

    holder.extra = std::make_unique<Leaf>("extra");
    holder.extra->value = 4;
    // Saved without the child, the snapshot is refused once the child stands.
    CHECK_FALSE(local::admits(holder, kHolderParts, absent, local::noValues()));
    NativeStateDigest stale;
    CHECK_FALSE(local::append(holder, kHolderParts, stale, &absent, local::noValues()));
    CHECK_FALSE(stale.checkedFinish().has_value());

    HolderSlots present;
    REQUIRE(local::save(holder, kHolderParts, present, local::noValues()));
    REQUIRE(present.extra.has_value());
    NativeStateDigest withChild;
    CHECK(local::append(holder, kHolderParts, withChild, static_cast<const HolderSlots*>(nullptr),
                        local::noValues()));
    NativeStateDigest flagThenChild;
    flagThenChild.flag(true);
    flagThenChild.integer(4);
    CHECK(withChild.checkedFinish() == flagThenChild.checkedFinish());
    NativeStateDigest fromSaved;
    CHECK(local::append(holder, kHolderParts, fromSaved, &present, local::noValues()));
    CHECK(fromSaved.checkedFinish() == withChild.checkedFinish());
    holder.extra->value = 8;
    REQUIRE(local::admits(holder, kHolderParts, present, local::noValues()));
    local::restore(holder, kHolderParts, present, local::noValues());
    CHECK(holder.extra->value == 4);
    // Saved with the child, the snapshot is refused once the child is gone.
    holder.extra.reset();
    CHECK_FALSE(local::admits(holder, kHolderParts, present, local::noValues()));
}

TEST_CASE("a roster writes its tag first and its owner's values where it names them",
          "[local-roster]") {
    Parent parent;
    parent.first.value = 1;
    parent.second.value = 2;
    parent.count = 3;
    NativeStateDigest expected;
    expected.string("Parent.native.1");
    expected.integer(1);
    expected.integer(2);
    expected.integer(3);
    const auto written = digestOf(parent, kParentParts, nullptr);
    REQUIRE(written.has_value());
    CHECK(written == expected.checkedFinish());
    // The same parts in another order write another stream.
    constexpr auto kReversed =
        local::parts(local::own(), local::child(&Parent::second, &Slots::second),
                     local::child(&Parent::first, &Slots::first), local::tag("Parent.native.1"));
    const auto reversed = digestOf(parent, kReversed, nullptr);
    REQUIRE(reversed.has_value());
    CHECK(reversed != written);
}

TEST_CASE("a save that fails stops the roster's save at that part", "[local-roster]") {
    Asked asked;
    Parent parent{&asked};
    parent.third.failsSave = true;
    constexpr auto kParts = local::parts(local::child(&Parent::first, &Slots::first),
                                         local::child(&Parent::third, &Slots::third),
                                         local::child(&Parent::second, &Slots::second));
    Slots saved;
    CHECK_FALSE(local::save(parent, kParts, saved, local::noValues()));
    CHECK(asked == Asked{"save first", "fail third"});
    // What the failed save never reached was never saved.
    CHECK(parent.first.canRestoreLocal(saved.first));
    CHECK_FALSE(parent.second.canRestoreLocal(saved.second));
}

TEST_CASE("a roster stops asking at the first part that refuses", "[local-roster]") {
    Asked asked;
    const Parent parent{&asked};
    constexpr auto kParts = local::parts(local::child(&Parent::first, &Slots::first),
                                         local::child(&Parent::second, &Slots::second),
                                         local::child(&Parent::third, &Slots::third));
    Slots saved;
    REQUIRE(local::save(parent, kParts, saved, local::noValues()));
    // The second leaf's slot holds another parent's snapshot.
    const Parent other;
    other.second.saveLocal(saved.second);
    asked.clear();
    CHECK_FALSE(local::admits(parent, kParts, saved, local::noValues()));
    CHECK(asked == Asked{"admit first", "admit second"});
}

TEST_CASE("a restore takes back every part in roster order", "[local-roster]") {
    Asked asked;
    Parent parent{&asked};
    parent.first.value = 1;
    parent.second.value = 2;
    parent.count = 3;
    Slots saved;
    REQUIRE(local::save(parent, kParentParts, saved, [&] { saved.count = parent.count; }));
    parent.first.value = 4;
    parent.second.value = 5;
    parent.count = 6;
    REQUIRE(local::admits(parent, kParentParts, saved, [] { return true; }));
    asked.clear();
    local::restore(parent, kParentParts, saved, [&] {
        asked.emplace_back("restore count");
        parent.count = saved.count;
    });
    CHECK(asked == Asked{"restore first", "restore second", "restore count"});
    CHECK(parent.first.value == 1);
    CHECK(parent.second.value == 2);
    CHECK(parent.count == 3);
}

TEST_CASE("each sample is visited, the rest carrying on after one spoils the digest",
          "[local-roster]") {
    Asked asked;
    Parent parent{&asked};
    parent.second.spoilsDigest = true;
    constexpr auto kSamples =
        local::parts(local::sample("first", local::child(&Parent::first, &Slots::first)),
                     local::sample("second", local::child(&Parent::second, &Slots::second)),
                     local::sample("third", local::child(&Parent::third, &Slots::third)));
    NativeStateDigest digest;
    std::vector<std::string_view> visited;
    // As the scene does: each sample measured on its own, a failure spoiling the digest.
    local::eachSample(kSamples, [&](std::string_view name, const auto& part) {
        visited.push_back(name);
        if (!local::append(parent, local::parts(part), digest, static_cast<const Slots*>(nullptr),
                           local::noValues())) {
            digest.invalidate();
        }
    });
    CHECK(visited == std::vector<std::string_view>{"first", "second", "third"});
    CHECK(asked == Asked{"append first", "append second", "append third"});
    CHECK_FALSE(digest.checkedFinish().has_value());
}

TEST_CASE("a snapshot hashes as the live owner it was saved from", "[local-roster]") {
    Parent parent;
    parent.first.value = 1;
    parent.second.value = 2;
    parent.count = 3;
    Slots saved;
    REQUIRE(local::save(parent, kParentParts, saved, [&] { saved.count = parent.count; }));
    const auto live = digestOf(parent, kParentParts, nullptr);
    REQUIRE(live.has_value());
    CHECK(digestOf(parent, kParentParts, &saved) == live);
    parent.second.value = 7;
    CHECK(digestOf(parent, kParentParts, nullptr) != live);
    CHECK(digestOf(parent, kParentParts, &saved) == live);
}

TEST_CASE("two owners reached through accessors come back with a value saved beside them",
          "[local-roster]") {
    Ceremonies ceremonies;
    ceremonies.promotion().value = 1;
    ceremonies.relics().value = 2;
    ceremonies.voice = 3.0;
    CeremonySlots saved;
    REQUIRE(local::save(ceremonies, kCeremonyParts, saved, local::noValues()));
    saved.voice = ceremonies.voice;
    ceremonies.promotion().value = 4;
    ceremonies.relics().value = 5;
    ceremonies.voice = 6.0;
    const Ceremonies foreign;
    CHECK_FALSE(local::admits(foreign, kCeremonyParts, saved, local::noValues()));
    REQUIRE(local::admits(ceremonies, kCeremonyParts, saved, local::noValues()));
    local::restore(ceremonies, kCeremonyParts, saved, local::noValues());
    ceremonies.voice = saved.voice;
    CHECK(ceremonies.promotion().value == 1);
    CHECK(ceremonies.relics().value == 2);
    CHECK(ceremonies.voice == 3.0);
    // Each is measured under its own name.
    std::vector<std::string_view> names;
    local::eachSample(kCeremonyParts,
                      [&](std::string_view name, const auto& /*part*/) { names.push_back(name); });
    CHECK(names == std::vector<std::string_view>{"promotion", "relics"});
}

} // namespace
