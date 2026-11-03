#pragma once
// Just enough JSON for the chat protocol (shared/protocol/chat.md): objects, arrays,
// strings, numbers, booleans, null. Portable C++17 so it's tested on Linux too.
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fo::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value& operator[](const std::string& key) const;  // Null when missing
    std::string str(const std::string& key, const std::string& def = "") const;
    double num(const std::string& key, double def = 0) const;
    bool boolean(const std::string& key, bool def = false) const;
    bool is_object() const { return type == Type::Object; }
};

// Returns false on malformed input.
bool parse(const std::string& text, Value& out);

// Writer helpers: quote() escapes a string as a JSON string literal.
std::string quote(const std::string& s);
std::string number(double v);

}  // namespace fo::json
