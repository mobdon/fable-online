// Small JSON reader/writer for the launcher's gateway API. UTF-8 in, UTF-8 out.
#pragma once
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;        // array elements, or object values
    std::vector<std::string> keys;   // object keys, parallel to items

    // Lookups never throw: a missing key gives a Null value.
    const Value& operator[](const char* key) const;
    const Value& operator[](size_t i) const;
    size_t size() const { return items.size(); }
    bool is_null() const { return type == Null; }
    std::string str(const std::string& fallback = "") const { return type == String ? s : fallback; }
    double num(double fallback = 0) const { return type == Number ? n : fallback; }
    bool boolean(bool fallback = false) const { return type == Bool ? b : fallback; }
};

// Returns false (and leaves out Null) on malformed input.
bool parse(const std::string& text, Value& out);

// A JSON string literal, quotes included.
std::string quote(const std::string& s);

}  // namespace json
