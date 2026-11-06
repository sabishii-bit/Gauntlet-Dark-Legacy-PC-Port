#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/LocalBinding.h"

namespace {

using gdl::LocalBinding;

/** A stamp taken under `binding` as it stands. */
LocalBinding::Stamp stampOf(const LocalBinding& binding) {
    LocalBinding::Stamp stamp;
    binding.stamp(stamp);
    return stamp;
}

TEST_CASE("a binding admits each stamp it takes", "[local-binding]") {
    const LocalBinding binding;
    const LocalBinding::Stamp first = stampOf(binding);
    const LocalBinding::Stamp second = stampOf(binding);
    CHECK(binding.admits(first));
    CHECK(binding.admits(second));
}

TEST_CASE("a binding refuses another binding's stamps", "[local-binding]") {
    const LocalBinding first;
    const LocalBinding second;
    CHECK_FALSE(second.admits(stampOf(first)));
    CHECK_FALSE(first.admits(stampOf(second)));
    // A binding made where a destroyed one stood refuses what the destroyed one stamped.
    std::optional<LocalBinding> slot{std::in_place};
    const LocalBinding::Stamp ofDestroyed = stampOf(*slot);
    slot.reset();
    slot.emplace();
    CHECK_FALSE(slot->admits(ofDestroyed));
}

TEST_CASE("a rebind refuses every stamp taken before it", "[local-binding]") {
    LocalBinding binding;
    const LocalBinding::Stamp before = stampOf(binding);
    binding.rebind();
    CHECK_FALSE(binding.admits(before));
    const LocalBinding::Stamp after = stampOf(binding);
    CHECK(binding.admits(after));
    binding.rebind();
    CHECK_FALSE(binding.admits(after));
    CHECK_FALSE(binding.admits(before));
}

TEST_CASE("a move refuses the stamps taken before it on both sides", "[local-binding]") {
    const auto source = std::make_unique<LocalBinding>();
    const LocalBinding::Stamp before = stampOf(*source);
    const LocalBinding moved{std::move(*source)};
    CHECK_FALSE(moved.admits(before));
    CHECK_FALSE(source->admits(before));
    // Each side goes on as a binding of its own.
    const LocalBinding::Stamp ofMoved = stampOf(moved);
    const LocalBinding::Stamp ofSource = stampOf(*source);
    CHECK(moved.admits(ofMoved));
    CHECK(source->admits(ofSource));
    CHECK_FALSE(moved.admits(ofSource));
    CHECK_FALSE(source->admits(ofMoved));
}

TEST_CASE("a move assignment refuses the stamps both sides took before it", "[local-binding]") {
    const auto source = std::make_unique<LocalBinding>();
    LocalBinding target;
    const LocalBinding::Stamp ofSource = stampOf(*source);
    const LocalBinding::Stamp ofTarget = stampOf(target);
    target = std::move(*source);
    CHECK_FALSE(target.admits(ofSource));
    CHECK_FALSE(target.admits(ofTarget));
    CHECK_FALSE(source->admits(ofSource));
    // The target keeps its own identity, apart from the source's.
    const LocalBinding::Stamp ofTargetAfter = stampOf(target);
    CHECK(target.admits(ofTargetAfter));
    CHECK_FALSE(source->admits(ofTargetAfter));
}

TEST_CASE("no binding admits a default stamp or an unstamped one, and a copy keeps its own",
          "[local-binding]") {
    const LocalBinding binding;
    CHECK_FALSE(binding.admits(LocalBinding::Stamp{}));
    LocalBinding::Stamp stamp = stampOf(binding);
    REQUIRE(binding.admits(stamp));
    const LocalBinding::Stamp copy = stamp;
    LocalBinding::unstamp(stamp);
    CHECK_FALSE(binding.admits(stamp));
    // A snapshot copied before carries a stamp of its own.
    CHECK(binding.admits(copy));
}

TEST_CASE("a binding is moved but never copied, and only its move constructor may throw",
          "[local-binding]") {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<LocalBinding>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<LocalBinding>);
    STATIC_REQUIRE(std::is_move_constructible_v<LocalBinding>);
    STATIC_REQUIRE_FALSE(std::is_nothrow_move_constructible_v<LocalBinding>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<LocalBinding>);
    STATIC_REQUIRE(std::is_copy_constructible_v<LocalBinding::Stamp>);
}

} // namespace
