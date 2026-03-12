#include "pretokenized_doc.h"
#include <cstring>

std::string PreTokenizedDoc::serialize() const {
    std::string buf;
    // Reserve estimated size: 8 (header) + fields * ~200 bytes avg
    buf.reserve(8 + fields.size() * 200);

    // Header
    uint32_t magic = MAGIC;
    uint16_t version = VERSION;
    uint16_t num_fields = static_cast<uint16_t>(fields.size());
    buf.append(reinterpret_cast<const char*>(&magic), 4);
    buf.append(reinterpret_cast<const char*>(&version), 2);
    buf.append(reinterpret_cast<const char*>(&num_fields), 2);

    for(const auto& field : fields) {
        uint16_t name_len = static_cast<uint16_t>(field.field_name.size());
        buf.append(reinterpret_cast<const char*>(&name_len), 2);
        buf.append(field.field_name);
        buf.append(reinterpret_cast<const char*>(&field.field_type), 1);

        if(field.field_type == 0) {
            // String tokens
            uint32_t num_tokens = static_cast<uint32_t>(field.tokens.size());
            buf.append(reinterpret_cast<const char*>(&num_tokens), 4);

            for(const auto& tok_pair : field.tokens) {
                uint16_t tok_len = static_cast<uint16_t>(tok_pair.first.size());
                buf.append(reinterpret_cast<const char*>(&tok_len), 2);
                buf.append(tok_pair.first);

                uint16_t num_offsets = static_cast<uint16_t>(tok_pair.second.size());
                buf.append(reinterpret_cast<const char*>(&num_offsets), 2);
                buf.append(reinterpret_cast<const char*>(tok_pair.second.data()),
                          tok_pair.second.size() * sizeof(uint32_t));
            }
        } else if(field.field_type == 1) {
            // Sort value
            buf.append(reinterpret_cast<const char*>(&field.sort_value), 8);
        } else if(field.field_type == 2) {
            // Facet value
            buf.append(reinterpret_cast<const char*>(&field.facet_hash), 8);
            uint16_t fv_len = static_cast<uint16_t>(field.facet_value.size());
            buf.append(reinterpret_cast<const char*>(&fv_len), 2);
            buf.append(field.facet_value);
        }
    }

    return buf;
}

bool PreTokenizedDoc::deserialize(const std::string& data, PreTokenizedDoc& out) {
    if(data.size() < 8) return false;

    const char* ptr = data.data();
    const char* end = ptr + data.size();

    uint32_t magic;
    memcpy(&magic, ptr, 4); ptr += 4;
    if(magic != MAGIC) return false;

    uint16_t version;
    memcpy(&version, ptr, 2); ptr += 2;
    if(version != VERSION) return false;

    uint16_t num_fields;
    memcpy(&num_fields, ptr, 2); ptr += 2;

    out.fields.resize(num_fields);

    for(uint16_t i = 0; i < num_fields; i++) {
        if(ptr + 2 > end) return false;
        uint16_t name_len;
        memcpy(&name_len, ptr, 2); ptr += 2;

        if(ptr + name_len + 1 > end) return false;
        out.fields[i].field_name.assign(ptr, name_len); ptr += name_len;
        out.fields[i].field_type = *reinterpret_cast<const uint8_t*>(ptr); ptr += 1;

        if(out.fields[i].field_type == 0) {
            if(ptr + 4 > end) return false;
            uint32_t num_tokens;
            memcpy(&num_tokens, ptr, 4); ptr += 4;

            out.fields[i].tokens.resize(num_tokens);
            for(uint32_t t = 0; t < num_tokens; t++) {
                if(ptr + 2 > end) return false;
                uint16_t tok_len;
                memcpy(&tok_len, ptr, 2); ptr += 2;

                if(ptr + tok_len + 2 > end) return false;
                out.fields[i].tokens[t].first.assign(ptr, tok_len); ptr += tok_len;

                uint16_t num_offsets;
                memcpy(&num_offsets, ptr, 2); ptr += 2;

                size_t offsets_bytes = num_offsets * sizeof(uint32_t);
                if(ptr + offsets_bytes > end) return false;
                out.fields[i].tokens[t].second.resize(num_offsets);
                memcpy(out.fields[i].tokens[t].second.data(), ptr, offsets_bytes);
                ptr += offsets_bytes;
            }
        } else if(out.fields[i].field_type == 1) {
            if(ptr + 8 > end) return false;
            memcpy(&out.fields[i].sort_value, ptr, 8); ptr += 8;
        } else if(out.fields[i].field_type == 2) {
            if(ptr + 10 > end) return false;
            memcpy(&out.fields[i].facet_hash, ptr, 8); ptr += 8;
            uint16_t fv_len;
            memcpy(&fv_len, ptr, 2); ptr += 2;
            if(ptr + fv_len > end) return false;
            out.fields[i].facet_value.assign(ptr, fv_len); ptr += fv_len;
        }
    }

    return true;
}

bool PreTokenizedDoc::is_valid(const std::string& data) {
    if(data.size() < 8) return false;
    uint32_t magic;
    memcpy(&magic, data.data(), 4);
    return magic == MAGIC;
}
