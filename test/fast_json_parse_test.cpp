#include <gtest/gtest.h>
#include "fast_json_parse.h"

class FastJsonParseTest : public ::testing::Test {};

TEST_F(FastJsonParseTest, ParsesIdenticalToNlohmann) {
    // Test with actual account-like documents
    std::vector<std::string> test_docs = {
        R"({"id":"0","account_id":"acc_abc123def4567","entity_id":"ent_xyz789abc1234","team_id":"team_001","merchant_name":"Starbucks","type":"depository","status":"active","created_at":1700000000,"updated_at":1700000001})",
        R"({"id":"1","account_id":"acc_000000000000000","entity_id":"","team_id":"team_002","merchant_name":"","type":"credit","status":"closed","created_at":0,"updated_at":0})",
        R"({"id":"2","nested":{"a":1,"b":[1,2,3]},"arr":["x","y"],"bool_field":true,"null_field":null})",
    };

    for(const auto& doc_str : test_docs) {
        auto nlohmann_result = nlohmann::json::parse(doc_str);
        auto simdjson_result = FastJsonParser::parse(doc_str);
        ASSERT_EQ(nlohmann_result, simdjson_result)
            << "Mismatch for: " << doc_str;
    }
}

TEST_F(FastJsonParseTest, ParsesWithPreAllocatedParser) {
    simdjson::ondemand::parser parser;
    std::string doc_str = R"({"id":"0","name":"test","value":42})";
    simdjson::padded_string padded(doc_str);

    auto nlohmann_result = nlohmann::json::parse(doc_str);
    auto simdjson_result = FastJsonParser::parse(parser, padded);

    ASSERT_EQ(nlohmann_result, simdjson_result);
}

TEST_F(FastJsonParseTest, HandlesEdgeCases) {
    // Empty object
    ASSERT_EQ(FastJsonParser::parse("{}"), nlohmann::json::object());

    // Large integers
    auto result = FastJsonParser::parse(R"({"big":9223372036854775807})");
    ASSERT_EQ(result["big"].get<int64_t>(), INT64_MAX);

    // Unicode strings
    auto unicode_result = FastJsonParser::parse(R"({"name":"日本語テスト"})");
    ASSERT_EQ(unicode_result["name"].get<std::string>(), "日本語テスト");

    // Floating point numbers
    auto float_result = FastJsonParser::parse(R"({"pi":3.14159})");
    ASSERT_DOUBLE_EQ(float_result["pi"].get<double>(), 3.14159);

    // Boolean and null
    auto mixed = FastJsonParser::parse(R"({"t":true,"f":false,"n":null})");
    ASSERT_EQ(mixed["t"].get<bool>(), true);
    ASSERT_EQ(mixed["f"].get<bool>(), false);
    ASSERT_TRUE(mixed["n"].is_null());
}

TEST_F(FastJsonParseTest, HandlesNestedStructures) {
    std::string nested_doc = R"({
        "level1": {
            "level2": {
                "level3": [1, 2, {"deep": true}]
            }
        },
        "array_of_objects": [
            {"a": 1},
            {"b": 2}
        ]
    })";

    auto nlohmann_result = nlohmann::json::parse(nested_doc);
    auto simdjson_result = FastJsonParser::parse(nested_doc);
    ASSERT_EQ(nlohmann_result, simdjson_result);
}

TEST_F(FastJsonParseTest, HandlesEmptyArraysAndStrings) {
    std::string doc = R"({"empty_arr":[],"empty_str":"","nested_empty":{"arr":[]}})";
    auto nlohmann_result = nlohmann::json::parse(doc);
    auto simdjson_result = FastJsonParser::parse(doc);
    ASSERT_EQ(nlohmann_result, simdjson_result);
}
