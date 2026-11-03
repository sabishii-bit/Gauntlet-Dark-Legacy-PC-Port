#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleListings.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Words = std::vector<std::string>;

/** What `list` answers words no listing answers, for the test's listings. */
constexpr std::string_view kUsage = "list fruit [kind]  the fruit, or one kind\n"
                                    "list nuts          the nuts";

/** Listings of the test's own: fruit twice, the second adding to the first, and nuts. */
struct Listed {
    ConsoleListings listings;
    DevConsole console;
    std::vector<Words> asked; ///< the words after its own each fruit listing was given

    Listed() {
        console.addCommand(listCommand(listings));
        listings.add(std::make_unique<LambdaListing>(
            "fruit", ConsoleListing::Row{"list fruit [kind]", "the fruit, or one kind"},
            [this](ConsoleListing::Arguments rest) {
                asked.emplace_back(rest.begin(), rest.end());
                return Words{"apple", "pear"};
            }));
        listings.add(std::make_unique<LambdaListing>(
            "nuts", ConsoleListing::Row{"list nuts", "the nuts"},
            [](ConsoleListing::Arguments /*rest*/) { return Words{"walnut"}; }));
        listings.add(std::make_unique<LambdaListing>(
            "Fruit", std::nullopt,
            [](ConsoleListing::Arguments /*rest*/) { return Words{"plum"}; }));
    }
};
} // namespace

TEST_CASE("list says what each listing of its word says, in the order they were registered",
          "[dev-console][commands]") {
    Listed fixture;
    CHECK(test::said(fixture.console, "list fruit") == "apple\npear\nplum");
    CHECK(test::said(fixture.console, "list NUTS") == "walnut");
    // The words after the listing's own are its, as typed.
    CHECK(test::said(fixture.console, "list Fruit Red x") == "apple\npear\nplum");
    CHECK(fixture.asked == std::vector<Words>{{}, {"Red", "x"}});
    // Words no listing answers, or none, are answered with the usage.
    CHECK(test::said(fixture.console, "list") == test::shownAs(kUsage));
    CHECK(test::said(fixture.console, "list seeds") == test::shownAs(kUsage));
    CHECK(fixture.listings.usage() == kUsage);
    CHECK(fixture.listings.lines(Words{"seeds"}) == Words{std::string(kUsage)});
}

TEST_CASE("list completes to each word listed once and offers its usage line once",
          "[dev-console][commands]") {
    Listed fixture;
    CHECK(fixture.listings.words() == Words{"fruit", "nuts"});
    const ConsoleCommand* list = fixture.console.commands().find("list");
    REQUIRE(list != nullptr);
    CHECK(list->completions(Words{""}) == Words{"fruit", "nuts"});
    CHECK(list->completions(Words{"f"}) == Words{"fruit", "nuts"});
    CHECK(list->completions(Words{"fruit", "r"}).empty());
    CHECK(list->name() == "list");
    CHECK(list->summary() == "maps, items or inventory: what there is");
    CHECK(list->reach() == ConsoleReach::Local);
    // A listing registered after the command is made still lists.
    fixture.listings.add(std::make_unique<LambdaListing>(
        "seeds", ConsoleListing::Row{"list seeds", "the seeds"},
        [](ConsoleListing::Arguments /*rest*/) { return Words{"pip"}; }));
    CHECK(test::said(fixture.console, "list seeds") == "pip");
    CHECK(list->completions(Words{""}) == Words{"fruit", "nuts", "seeds"});
    // With nothing listed, the usage is empty.
    const ConsoleListings none;
    CHECK(none.usage().empty());
    CHECK(none.words().empty());
}
