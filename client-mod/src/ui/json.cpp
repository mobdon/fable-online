#include "json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fo::json {
namespace {
const Value kNull;

struct Parser {
    const std::string& t;
    size_t i = 0;
    int depth = 0;

    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    i += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else cp = '?';
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = '?';
                }
                utf8(o, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Value& v) {
        if (++depth > 32) return false;
        ws();
        if (i >= t.size()) return false;
        char c = t[i];
        bool ok = false;
        if (c == '{') {
            v.type = Value::Type::Object;
            ++i; ws();
            if (i < t.size() && t[i] == '}') { ++i; ok = true; }
            else for (;;) {
                ws();
                std::string k;
                if (!string(k)) break;
                ws();
                if (i >= t.size() || t[i++] != ':') break;
                if (!value(v.obj[k])) break;
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; ok = true; }
                break;
            }
        } else if (c == '[') {
            v.type = Value::Type::Array;
            ++i; ws();
            if (i < t.size() && t[i] == ']') { ++i; ok = true; }
            else for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back())) break;
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; ok = true; }
                break;
            }
        } else if (c == '"') {
            v.type = Value::Type::String;
            ok = string(v.s);
        } else if (lit("true")) { v.type = Value::Type::Bool; v.b = true; ok = true; }
        else if (lit("false")) { v.type = Value::Type::Bool; ok = true; }
        else if (lit("null")) { ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            v.n = std::strtod(start, &end);
            if (end == start) return false;
            i += end - start;
            v.type = Value::Type::Number;
            ok = std::isfinite(v.n);
        }
        --depth;
        return ok;
    }
};
}  // namespace

const Value& Value::operator[](const std::string& key) const {
    if (type != Type::Object) return kNull;
    auto it = obj.find(key);
    return it == obj.end() ? kNull : it->second;
}
std::string Value::str(const std::string& key, const std::string& def) const {
    const Value& v = (*this)[key];
    return v.type == Type::String ? v.s : def;
}
double Value::num(const std::string& key, double def) const {
    const Value& v = (*this)[key];
    return v.type == Type::Number ? v.n : def;
}
bool Value::boolean(const std::string& key, bool def) const {
    const Value& v = (*this)[key];
    return v.type == Type::Bool ? v.b : def;
}

bool parse(const std::string& text, Value& out) {
    out = Value();
    Parser p{text};
    if (!p.value(out)) return false;
    p.ws();
    return p.i == text.size();
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o + "\"";
}

std::string number(double v) {
    if (!std::isfinite(v)) return "0";
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}
}  // namespace fo::json
