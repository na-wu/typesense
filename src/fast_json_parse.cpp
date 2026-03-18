#include "fast_json_parse.h"

nlohmann::json FastJsonParser::parse(const std::string& json_str) {
    thread_local simdjson::ondemand::parser parser;
    auto padded = simdjson::padded_string(json_str);
    auto doc = parser.iterate(padded);
    if(doc.error()) {
        // Fallback to nlohmann
        return nlohmann::json::parse(json_str);
    }
    return convert_object(doc.get_object().value());
}

nlohmann::json FastJsonParser::parse(simdjson::ondemand::parser& parser,
                                     simdjson::padded_string_view json_str) {
    auto doc = parser.iterate(json_str);
    if(doc.error()) {
        return nlohmann::json::parse(std::string(json_str.data(), json_str.length()));
    }
    return convert_object(doc.get_object().value());
}

nlohmann::json FastJsonParser::convert_object(simdjson::ondemand::object obj) {
    nlohmann::json result = nlohmann::json::object();
    for(auto field : obj) {
        std::string_view key = field.unescaped_key().value();
        result[std::string(key)] = convert(field.value());
    }
    return result;
}

nlohmann::json FastJsonParser::convert_array(simdjson::ondemand::array arr) {
    nlohmann::json result = nlohmann::json::array();
    for(auto element : arr) {
        result.push_back(convert(element.value()));
    }
    return result;
}

nlohmann::json FastJsonParser::convert(simdjson::ondemand::value val) {
    switch(val.type().value()) {
        case simdjson::ondemand::json_type::object:
            return convert_object(val.get_object().value());
        case simdjson::ondemand::json_type::array:
            return convert_array(val.get_array().value());
        case simdjson::ondemand::json_type::string:
            return std::string(val.get_string().value());
        case simdjson::ondemand::json_type::number: {
            // Try int64 first, fall back to double
            auto as_int = val.get_int64();
            if(!as_int.error()) return as_int.value();
            auto as_uint = val.get_uint64();
            if(!as_uint.error()) return as_uint.value();
            return val.get_double().value();
        }
        case simdjson::ondemand::json_type::boolean:
            return val.get_bool().value();
        case simdjson::ondemand::json_type::null:
            return nullptr;
        default:
            return nullptr;
    }
}
