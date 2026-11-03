#include "json.h"
#include <cstdlib>
#include <cstring>

namespace json {
namespace {
const Value kNull;

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (size_t(end - p) < n || std::memcmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { o += c; continue; }
            if (p >= end) return false;
            switch (*p++) {
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
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
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
        if (++depth > 64) return false;
        ws();
        if (p >= end) return false;
        bool ok = true;
        char c = *p;
        if (c == '{') {
            v.type = Value::Object;
            ++p; ws();
            if (p < end && *p == '}') { ++p; }
            else for (;;) {
                std::string k;
                ws();
                if (!string(k)) { ok = false; break; }
                ws();
                if (p >= end || *p++ != ':') { ok = false; break; }
                Value child;
                if (!value(child)) { ok = false; break; }
                v.keys.push_back(std::move(k));
                v.items.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v.type = Value::Array;
            ++p; ws();
            if (p < end && *p == ']') { ++p; }
            else for (;;) {
                Value child;
                if (!value(child)) { ok = false; break; }
                v.items.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            v.type = Value::String;
            ok = string(v.s);
        } else if (lit("true")) { v.type = Value::Bool; v.b = true; }
        else if (lit("false")) { v.type = Value::Bool; v.b = false; }
        else if (lit("null")) { v.type = Value::Null; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            std::string num;
            while (p < end && (std::strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) num += *p++;
            char* stop = nullptr;
            v.type = Value::Number;
            v.n = std::strtod(num.c_str(), &stop);
            ok = stop && *stop == 0;
        } else ok = false;
        --depth;
        return ok;
    }
};
}  // namespace

const Value& Value::operator[](const char* key) const {
    if (type != Object) return kNull;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return items[i];
    return kNull;
}

const Value& Value::operator[](size_t i) const {
    return (type == Array && i < items.size()) ? items[i] : kNull;
}

bool parse(const std::string& text, Value& out) {
    Parser ps{text.data(), text.data() + text.size()};
    Value v;
    if (!ps.value(v)) { out = Value(); return false; }
    ps.ws();
    if (ps.p != ps.end) { out = Value(); return false; }
    out = std::move(v);
    return true;
}

std::string quote(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { o += "\\u00"; o += hex[c >> 4]; o += hex[c & 15]; }
                else o += char(c);
        }
    }
    return o + "\"";
}

}  // namespace json
