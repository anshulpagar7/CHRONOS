#include "chronos/api/json.h"

#include <gtest/gtest.h>

namespace chronos::api {
namespace {

TEST(Json, BuildsAndDumpsCompactly) {
    JsonValue v = JsonValue::object();
    v.set("name", "encode").set("priority", 5).set("ok", true).set("gone", nullptr);
    v.set("tags", JsonValue::array().push("a").push("b"));

    EXPECT_EQ(v.dump(),
              R"({"gone":null,"name":"encode","ok":true,"priority":5,"tags":["a","b"]})");
}

TEST(Json, RoundTripsNestedStructures) {
    const std::string text =
        R"({"a":[1,2.5,-3],"b":{"c":"hi","d":false},"e":null})";
    const JsonValue v = JsonValue::parse(text);

    EXPECT_TRUE(v.is_object());
    EXPECT_EQ(v.at("a").size(), 3u);
    EXPECT_DOUBLE_EQ(v.at("a").as_array()[1].as_number(), 2.5);
    EXPECT_DOUBLE_EQ(v.at("a").as_array()[2].as_number(), -3.0);
    EXPECT_EQ(v.at("b").at("c").as_string(), "hi");
    EXPECT_FALSE(v.at("b").at("d").as_bool());
    EXPECT_TRUE(v.at("e").is_null());
    EXPECT_EQ(JsonValue::parse(v.dump()).dump(), v.dump());
}

TEST(Json, EscapesAndUnescapesStrings) {
    JsonValue v = JsonValue::object();
    v.set("s", "line1\nline2\t\"quoted\" \\ end");
    const std::string dumped = v.dump();
    EXPECT_EQ(JsonValue::parse(dumped).at("s").as_string(),
              "line1\nline2\t\"quoted\" \\ end");

    EXPECT_EQ(JsonValue::parse(R"("\u0041\u00e9")").as_string(), "A\xC3\xA9");
}

TEST(Json, IntegersStayIntegersInOutput) {
    JsonValue v = JsonValue::object();
    v.set("id", std::uint64_t{123456789});
    v.set("frac", 0.25);
    const std::string dumped = v.dump();
    EXPECT_NE(dumped.find("\"id\":123456789"), std::string::npos);
    EXPECT_NE(dumped.find("\"frac\":0.25"), std::string::npos);
}

TEST(Json, MissingKeysReadAsNullSafely) {
    const JsonValue v = JsonValue::parse(R"({"present":1})");
    EXPECT_TRUE(v.contains("present"));
    EXPECT_FALSE(v.contains("absent"));
    EXPECT_TRUE(v.at("absent").is_null());
    EXPECT_TRUE(v.at("absent").at("deeper").is_null());  // Chains safely.
}

TEST(Json, RejectsMalformedInput) {
    EXPECT_THROW(JsonValue::parse(""), std::runtime_error);
    EXPECT_THROW(JsonValue::parse("{"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse(R"({"a":1,})"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse(R"({"a" 1})"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse("[1,2 3]"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse("tru"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse("1 2"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse("\"unterminated"), std::runtime_error);
    EXPECT_THROW(JsonValue::parse("--5"), std::runtime_error);
}

TEST(Json, RejectsPathologicalNesting) {
    std::string deep(100, '[');
    deep += std::string(100, ']');
    EXPECT_THROW(JsonValue::parse(deep), std::runtime_error);
}

}  // namespace
}  // namespace chronos::api
