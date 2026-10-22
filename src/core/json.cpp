// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cutefish::backup {

Json::Json(bool value)
    : m_type(Type::Bool)
    , m_bool(value)
{
}

Json::Json(double value)
    : m_type(Type::Number)
    , m_number(value)
{
}

Json::Json(uint64_t value)
    : m_type(Type::Number)
    , m_number(double(value))
{
}

Json::Json(int64_t value)
    : m_type(Type::Number)
    , m_number(double(value))
{
}

Json::Json(int value)
    : m_type(Type::Number)
    , m_number(value)
{
}

Json::Json(const char* value)
    : m_type(Type::String)
    , m_string(value)
{
}

Json::Json(std::string value)
    : m_type(Type::String)
    , m_string(std::move(value))
{
}

Json Json::array()
{
    Json value;
    value.m_type = Type::Array;
    return value;
}

Json Json::object()
{
    Json value;
    value.m_type = Type::Object;
    return value;
}

bool Json::parse(const std::string& text, Json& into)
{
    size_t at = 0;
    skipSpace(text, at);
    if (at >= text.size() || !parseValue(text, at, into)) {
        into = Json();
        return false;
    }
    skipSpace(text, at);
    if (at != text.size()) {
        into = Json();
        return false;
    }
    return true;
}

Json Json::parse(const std::string& text)
{
    Json into;
    parse(text, into);
    return into;
}

bool Json::toBool(bool fallback) const
{
    return m_type == Type::Bool ? m_bool : fallback;
}

double Json::toNumber(double fallback) const
{
    return m_type == Type::Number ? m_number : fallback;
}

uint64_t Json::toUnsigned(uint64_t fallback) const
{
    return m_type == Type::Number && m_number >= 0 ? uint64_t(m_number) : fallback;
}

int64_t Json::toSigned(int64_t fallback) const
{
    return m_type == Type::Number ? int64_t(m_number) : fallback;
}

const std::string& Json::toString() const
{
    static const std::string empty;
    return m_type == Type::String ? m_string : empty;
}

const Json& Json::at(size_t index) const
{
    static const Json null;
    if (m_type == Type::Array && index < m_array.size())
        return m_array[index];
    return null;
}

size_t Json::size() const
{
    switch (m_type) {
    case Type::Array:
        return m_array.size();
    case Type::Object:
        return m_object.size();
    default:
        return 0;
    }
}

void Json::append(Json value)
{
    if (m_type != Type::Array)
        return;
    m_array.push_back(std::move(value));
}

const Json& Json::get(const std::string& key) const
{
    static const Json null;
    if (m_type != Type::Object)
        return null;
    const auto found = m_object.find(key);
    return found == m_object.end() ? null : found->second;
}

void Json::set(const std::string& key, Json value)
{
    if (m_type != Type::Object) {
        m_type = Type::Object;
        m_object.clear();
    }
    m_object[key] = std::move(value);
}

bool Json::contains(const std::string& key) const
{
    return m_type == Type::Object && m_object.find(key) != m_object.end();
}

const std::map<std::string, Json>& Json::members() const
{
    static const std::map<std::string, Json> empty;
    return m_type == Type::Object ? m_object : empty;
}

void Json::escape(const std::string& text, std::string& into) const
{
    into += '"';
    for (const char raw : text) {
        const unsigned char c = raw;
        switch (c) {
        case '"':
            into += "\\\"";
            break;
        case '\\':
            into += "\\\\";
            break;
        case '\b':
            into += "\\b";
            break;
        case '\f':
            into += "\\f";
            break;
        case '\n':
            into += "\\n";
            break;
        case '\r':
            into += "\\r";
            break;
        case '\t':
            into += "\\t";
            break;
        default:
            if (c < 0x20) {
                char text[8];
                std::snprintf(text, sizeof(text), "\\u%04x", c);
                into += text;
            } else {
                into += raw;
            }
        }
    }
    into += '"';
}

std::string Json::dump(bool pretty) const
{
    std::string out;
    // Recursive, so a named function rather than an auto lambda.
    struct Writer
    {
        bool pretty;
        std::string& out;

        void escape(const std::string& text) const
        {
            out += '"';
            for (const char raw : text) {
                const unsigned char c = raw;
                switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        char text[8];
                        std::snprintf(text, sizeof(text), "\\u%04x", c);
                        out += text;
                    } else {
                        out += raw;
                    }
                }
            }
            out += '"';
        }

        void write(const Json& value, int depth)
        {
            switch (value.m_type) {
            case Type::Null:
                out += "null";
                break;
            case Type::Bool:
                out += value.m_bool ? "true" : "false";
                break;
            case Type::Number: {
                char text[48];
                if (std::trunc(value.m_number) == value.m_number && std::abs(value.m_number) < 1e15) {
                    // Whole numbers stay whole, as a size or a count reads.
                    std::snprintf(text, sizeof(text), "%lld", (long long)value.m_number);
                } else {
                    std::snprintf(text, sizeof(text), "%.17g", value.m_number);
                }
                out += text;
                break;
            }
            case Type::String:
                escape(value.m_string);
                break;
            case Type::Array:
                if (value.m_array.empty()) {
                    out += "[]";
                    break;
                }
                out += pretty ? "[\n" : "[";
                for (size_t i = 0; i < value.m_array.size(); ++i) {
                    out.append(size_t(depth + 1) * 2, ' ');
                    write(value.m_array[i], depth + 1);
                    if (i + 1 < value.m_array.size())
                        out += pretty ? ",\n" : ",";
                    else if (pretty)
                        out += '\n';
                }
                out.append(size_t(depth) * 2, ' ');
                out += ']';
                break;
            case Type::Object:
                if (value.m_object.empty()) {
                    out += "{}";
                    break;
                }
                out += pretty ? "{\n" : "{";
                bool first = true;
                for (const auto& [key, member] : value.m_object) {
                    if (!first)
                        out += pretty ? ",\n" : ",";
                    first = false;
                    out.append(size_t(depth + 1) * 2, ' ');
                    escape(key);
                    out += pretty ? ": " : ":";
                    write(member, depth + 1);
                }
                if (pretty)
                    out += '\n';
                out.append(size_t(depth) * 2, ' ');
                out += '}';
                break;
            }
        }
    };
    Writer writer{pretty, out};
    writer.write(*this, 0);
    return out;
}

void Json::skipSpace(const std::string& text, size_t& at)
{
    while (at < text.size()) {
        const char c = text[at];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ++at;
        else
            break;
    }
}

static bool parseHex4(const std::string& text, size_t& at, unsigned& value)
{
    value = 0;
    for (int digit = 0; digit < 4; ++digit) {
        if (at >= text.size())
            return false;
        const char c = text[at++];
        value <<= 4;
        if (c >= '0' && c <= '9')
            value += unsigned(c - '0');
        else if (c >= 'a' && c <= 'f')
            value += unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value += unsigned(c - 'A' + 10);
        else
            return false;
    }
    return true;
}

static void appendUtf8(unsigned code, std::string& into)
{
    if (code < 0x80) {
        into += char(code);
    } else if (code < 0x800) {
        into += char(0xC0 | (code >> 6));
        into += char(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        into += char(0xE0 | (code >> 12));
        into += char(0x80 | ((code >> 6) & 0x3F));
        into += char(0x80 | (code & 0x3F));
    } else {
        into += char(0xF0 | (code >> 18));
        into += char(0x80 | ((code >> 12) & 0x3F));
        into += char(0x80 | ((code >> 6) & 0x3F));
        into += char(0x80 | (code & 0x3F));
    }
}

static bool parseString(const std::string& text, size_t& at, std::string& into)
{
    if (at >= text.size() || text[at] != '"')
        return false;
    ++at;
    into.clear();
    while (at < text.size()) {
        const char c = text[at++];
        if (c == '"')
            return true;
        if (c != '\\') {
            into += c;
            continue;
        }
        if (at >= text.size())
            return false;
        const char escape = text[at++];
        switch (escape) {
        case '"':
            into += '"';
            break;
        case '\\':
            into += '\\';
            break;
        case '/':
            into += '/';
            break;
        case 'b':
            into += '\b';
            break;
        case 'f':
            into += '\f';
            break;
        case 'n':
            into += '\n';
            break;
        case 'r':
            into += '\r';
            break;
        case 't':
            into += '\t';
            break;
        case 'u': {
            unsigned code = 0;
            if (!parseHex4(text, at, code))
                return false;
            if (code >= 0xD800 && code <= 0xDBFF && at + 1 < text.size() && text[at] == '\\'
                && text[at + 1] == 'u') {
                at += 2;
                unsigned low = 0;
                if (!parseHex4(text, at, low) || low < 0xDC00 || low > 0xDFFF)
                    return false;
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            appendUtf8(code, into);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

bool Json::parseValue(const std::string& text, size_t& at, Json& into)
{
    if (at >= text.size())
        return false;
    const char c = text[at];
    if (c == '{') {
        ++at;
        into = Json::object();
        skipSpace(text, at);
        if (at < text.size() && text[at] == '}') {
            ++at;
            return true;
        }
        while (true) {
            skipSpace(text, at);
            std::string key;
            if (!parseString(text, at, key))
                return false;
            skipSpace(text, at);
            if (at >= text.size() || text[at] != ':')
                return false;
            ++at;
            skipSpace(text, at);
            Json value;
            if (!parseValue(text, at, value))
                return false;
            into.m_object[key] = std::move(value);
            skipSpace(text, at);
            if (at < text.size() && text[at] == ',') {
                ++at;
                continue;
            }
            if (at < text.size() && text[at] == '}') {
                ++at;
                return true;
            }
            return false;
        }
    }
    if (c == '[') {
        ++at;
        into = Json::array();
        skipSpace(text, at);
        if (at < text.size() && text[at] == ']') {
            ++at;
            return true;
        }
        while (true) {
            skipSpace(text, at);
            Json value;
            if (!parseValue(text, at, value))
                return false;
            into.m_array.push_back(std::move(value));
            skipSpace(text, at);
            if (at < text.size() && text[at] == ',') {
                ++at;
                continue;
            }
            if (at < text.size() && text[at] == ']') {
                ++at;
                return true;
            }
            return false;
        }
    }
    if (c == '"') {
        std::string value;
        if (!parseString(text, at, value))
            return false;
        into = Json(std::move(value));
        return true;
    }
    if (c == 't' && text.compare(at, 4, "true") == 0) {
        at += 4;
        into = Json(true);
        return true;
    }
    if (c == 'f' && text.compare(at, 5, "false") == 0) {
        at += 5;
        into = Json(false);
        return true;
    }
    if (c == 'n' && text.compare(at, 4, "null") == 0) {
        at += 4;
        into = Json();
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const size_t start = at;
        if (text[at] == '-')
            ++at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9')
            ++at;
        if (at < text.size() && text[at] == '.') {
            ++at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                ++at;
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-'))
                ++at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                ++at;
        }
        into = Json(std::strtod(text.substr(start, at - start).c_str(), nullptr));
        return at > start;
    }
    return false;
}

} // namespace cutefish::backup
