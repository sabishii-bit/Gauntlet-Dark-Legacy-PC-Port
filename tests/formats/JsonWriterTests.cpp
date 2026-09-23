#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "formats/JsonWriter.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

TEST_CASE("nested objects and arrays are written with indentation", "[formats][json]") {
    JsonWriter json;
    json.beginObject();
    json.key("name").value("Title \"one\"\n");
    json.key("count").value(3);
    json.key("ratio").value(0.5);
    json.key("on").value(true);
    json.key("items").beginArray();
    json.value(1).value(2U);
    json.beginObject().key("k").value(static_cast<s64>(-7)).endObject();
    json.endArray();
    json.key("empty").beginArray().endArray();
    json.key("nothing").beginObject().endObject();
    json.endObject();

    const std::string expected = "{\n"
                                 "  \"name\": \"Title \\\"one\\\"\\n\",\n"
                                 "  \"count\": 3,\n"
                                 "  \"ratio\": 0.5,\n"
                                 "  \"on\": true,\n"
                                 "  \"items\": [\n"
                                 "    1,\n"
                                 "    2,\n"
                                 "    {\n"
                                 "      \"k\": -7\n"
                                 "    }\n"
                                 "  ],\n"
                                 "  \"empty\": [],\n"
                                 "  \"nothing\": {}\n"
                                 "}\n";
    REQUIRE(json.take() == expected);
    REQUIRE(json.text().empty());
}

TEST_CASE("control characters and non-finite numbers are escaped", "[formats][json]") {
    JsonWriter json;
    json.beginArray()
        .value(std::string("a\x01"
                           "b"))
        .value(std::numeric_limits<f64>::infinity())
        .endArray();
    REQUIRE(json.take() == "[\n  \"a\\u0001b\",\n  null\n]\n");
}

} // namespace
