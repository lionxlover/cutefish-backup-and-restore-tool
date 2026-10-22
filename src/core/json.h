// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cutefish::backup {

// A JSON value: null, true, false, a number, a string, an array or an
// object. Written for the machine-talk of the tool — the meta file beside an
// archive, and the progress events the CLI prints for the window — so it
// covers the whole grammar without carrying a library around. A number is
// kept as the double it parses to, as the sizes and times it carries are all
// whole numbers far below the range a double keeps exact.
class Json
{
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Json() = default;
    Json(bool value);
    Json(double value);
    Json(uint64_t value);
    Json(int64_t value);
    Json(int value);
    Json(const char* value);
    Json(std::string value);

    static Json array();
    static Json object();
    // Parses a whole document; null and false on a syntax error.
    static bool parse(const std::string& text, Json& into);
    static Json parse(const std::string& text);

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool fallback = false) const;
    double toNumber(double fallback = 0) const;
    uint64_t toUnsigned(uint64_t fallback = 0) const;
    int64_t toSigned(int64_t fallback = 0) const;
    const std::string& toString() const;

    // Array access; out of range is a null.
    const Json& at(size_t index) const;
    size_t size() const;
    void append(Json value);

    // Object access; a missing key is a null. Setting on a non-object
    // makes it one.
    const Json& get(const std::string& key) const;
    void set(const std::string& key, Json value);
    bool contains(const std::string& key) const;
    const std::map<std::string, Json>& members() const;

    // One line, without the object's member order or number formatting
    // mattering; pretty for the meta file a person may open.
    std::string dump(bool pretty = false) const;

private:
    void escape(const std::string& text, std::string& into) const;
    static void skipSpace(const std::string& text, size_t& at);
    static bool parseValue(const std::string& text, size_t& at, Json& into);

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<Json> m_array;
    std::map<std::string, Json> m_object;
};

} // namespace cutefish::backup
