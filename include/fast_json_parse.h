#pragma once
#include <string>
#include "json.hpp"  // nlohmann
#include "simdjson.h"

// Parse a JSON string using simdjson and convert to nlohmann::json.
// Falls back to nlohmann::json::parse() on error.
class FastJsonParser {
public:
    // Thread-local parser for reuse (simdjson recommends reusing parser objects)
    static nlohmann::json parse(const std::string& json_str);

    // Parse with a pre-allocated parser (for batch use in a loop)
    static nlohmann::json parse(simdjson::ondemand::parser& parser,
                                simdjson::padded_string_view json_str);

private:
    // Recursively convert a simdjson value to nlohmann::json
    static nlohmann::json convert(simdjson::ondemand::value val);
    static nlohmann::json convert_object(simdjson::ondemand::object obj);
    static nlohmann::json convert_array(simdjson::ondemand::array arr);
};
