#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

struct TokenizedField {
    std::string field_name;
    uint8_t field_type;  // 0=string_tokens, 1=sort_value, 2=facet_value

    // For string fields: token -> offsets
    std::vector<std::pair<std::string, std::vector<uint32_t>>> tokens;

    // For sort/numeric fields
    int64_t sort_value = 0;

    // For facet fields
    uint64_t facet_hash = 0;
    std::string facet_value;
};

struct PreTokenizedDoc {
    static constexpr uint32_t MAGIC = 0x54534F4B;  // "TSOK"
    static constexpr uint16_t VERSION = 1;

    std::vector<TokenizedField> fields;

    // Serialize to binary
    std::string serialize() const;

    // Deserialize from binary. Returns false on invalid data.
    static bool deserialize(const std::string& data, PreTokenizedDoc& out);

    // Check if binary data has valid magic/version
    static bool is_valid(const std::string& data);
};
