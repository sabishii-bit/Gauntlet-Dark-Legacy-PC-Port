#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/platform/Input.h"

#include "game/config/BindingNames.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Json = nlohmann::json;

/** The message of the FormatError `read` throws; empty when it returns or throws another. */
template <class Read> std::string refusalOf(const Read& read) {
    try {
        read();
    } catch (const FormatError& refusal) {
        return refusal.what();
    } catch (...) {
        return {};
    }
    return {};
}

TEST_CASE("a shared list reads on past a name it does not know, where a profile's refuses it",
          "[game][config][binding-names]") {
    const auto keyList = Json::array({"I", "Nope", "Q"});
    const auto buttonList = Json::array({"Y", "Nope", "B"});
    std::vector<Key> keys{Key::T};
    readKeyNames(keyList, "up", UnknownBinding::WarnAndSkip, false, keys);
    CHECK(keys == std::vector{Key::I, Key::Q});
    std::vector<PadButton> buttons{PadButton::A};
    readButtonNames(buttonList, "up", UnknownBinding::WarnAndSkip, buttons);
    CHECK(buttons == std::vector{PadButton::Y, PadButton::B});

    // Refused, a list keeps the names read before the one it does not know.
    keys = {Key::T};
    CHECK(refusalOf([&] { readKeyNames(keyList, "up", UnknownBinding::Refuse, false, keys); }) ==
          "unknown control key");
    CHECK(keys == std::vector{Key::I});
    buttons = {PadButton::A};
    CHECK(refusalOf([&] { readButtonNames(buttonList, "up", UnknownBinding::Refuse, buttons); }) ==
          "unknown control button");
    CHECK(buttons == std::vector{PadButton::Y});

    // An empty name names nothing.
    keys = {Key::T};
    readKeyNames(Json::array({""}), "up", UnknownBinding::WarnAndSkip, false, keys);
    CHECK(keys.empty());
    CHECK(refusalOf([&] {
              readKeyNames(Json::array({""}), "up", UnknownBinding::Refuse, false, keys);
          }) == "unknown control key");
}

TEST_CASE("Escape is read into a key list only where the list keeps it",
          "[game][config][binding-names]") {
    const auto list = Json::array({"Escape", "F5", "escape"});
    for (const auto policy : {UnknownBinding::WarnAndSkip, UnknownBinding::Refuse}) {
        std::vector<Key> keys;
        readKeyNames(list, "select", policy, false, keys);
        CHECK(keys == std::vector{Key::F5});
        readKeyNames(list, "shopExit", policy, true, keys);
        CHECK(keys == std::vector{Key::Escape, Key::F5, Key::Escape});
    }
}

TEST_CASE("a list is read afresh: one name for a list of it, null for none, an object for its "
          "values",
          "[game][config][binding-names]") {
    for (const auto policy : {UnknownBinding::WarnAndSkip, UnknownBinding::Refuse}) {
        std::vector<Key> keys{Key::T, Key::Z};
        readKeyNames(Json("Q"), "back", policy, false, keys);
        CHECK(keys == std::vector{Key::Q});
        readKeyNames(Json(), "back", policy, false, keys);
        CHECK(keys.empty());
        readKeyNames(Json::object({{"a", "I"}, {"b", "Q"}}), "back", policy, false, keys);
        CHECK(keys == std::vector{Key::I, Key::Q});
        readKeyNames(Json::array(), "back", policy, false, keys);
        CHECK(keys.empty());
        std::vector<PadButton> buttons{PadButton::A, PadButton::B};
        readButtonNames(Json("X"), "back", policy, buttons);
        CHECK(buttons == std::vector{PadButton::X});
        readButtonNames(Json(), "back", policy, buttons);
        CHECK(buttons.empty());
        readButtonNames(Json::object({{"a", "Y"}, {"b", "B"}}), "back", policy, buttons);
        CHECK(buttons == std::vector{PadButton::Y, PadButton::B});
    }
}

TEST_CASE("a name that is not a string stops a list part way, under either policy",
          "[game][config][binding-names]") {
    const auto list = Json::array({"Z", 5, "T"});
    for (const auto policy : {UnknownBinding::WarnAndSkip, UnknownBinding::Refuse}) {
        std::vector<Key> keys{Key::Q};
        CHECK_THROWS_AS(readKeyNames(list, "strafe", policy, false, keys), Json::type_error);
        CHECK(keys == std::vector{Key::Z});
        std::vector<PadButton> buttons{PadButton::A};
        CHECK_THROWS_AS(readButtonNames(Json::array({"Y", true, "B"}), "strafe", policy, buttons),
                        Json::type_error);
        CHECK(buttons == std::vector{PadButton::Y});
        CHECK_THROWS_AS(readKeyNames(Json(5), "strafe", policy, false, keys), Json::type_error);
        CHECK(keys.empty());
    }
}

} // namespace
