#pragma once

#include "keeper.h"

#include <charconv>
#include <cctype>
#include <cmath>
#include <variant>

namespace pk {
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    std::variant<std::monostate, bool, double, std::string, Array, Object> value;
    const Json* find(std::string_view key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) return nullptr;
        const auto it = object->find(key);
        return it == object->end() ? nullptr : &it->second;
    }
    const Json& at(std::string_view key) const {
        if (const auto* found = find(key)) return *found;
        throw std::runtime_error("JSON is missing required data.");
    }
    unsigned number() const {
        const auto n = std::get<double>(value);
        if (n < 0 || n > UINT_MAX || n != static_cast<unsigned>(n))
            throw std::runtime_error("Invalid integer.");
        return static_cast<unsigned>(n);
    }
    Text text() const { return wide(std::get<std::string>(value)); }
};

class Parser {
    std::string_view input;
    [[noreturn]] static void invalid() { throw std::runtime_error("Invalid JSON."); }
    bool take(char ch) {
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) input.remove_prefix(1);
        if (input.empty() || input.front() != ch) return false;
        input.remove_prefix(1);
        return true;
    }
    void need(char ch) { if (!take(ch)) invalid(); }
    wchar_t hex4() {
        if (input.size() < 4) invalid();
        unsigned result{};
        const auto end = input.data() + 4;
        const auto parsed = std::from_chars(input.data(), end, result, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end) invalid();
        input.remove_prefix(4);
        return static_cast<wchar_t>(result);
    }
    std::string string() {
        need('"');
        std::string out;
        while (!input.empty()) {
            auto ch = input.front();
            input.remove_prefix(1);
            if (ch == '"') return out;
            if (static_cast<unsigned char>(ch) < 32) invalid();
            if (ch != '\\') { out += ch; continue; }
            if (input.empty()) invalid();
            ch = input.front();
            input.remove_prefix(1);
            switch (ch) {
            case '"': case '\\': case '/': out += ch; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'r': out += '\r'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'u': {
                Text piece(1, hex4());
                if (piece[0] >= 0xD800 && piece[0] <= 0xDBFF) {
                    if (!input.starts_with("\\u")) invalid();
                    input.remove_prefix(2);
                    piece += hex4();
                }
                out += utf8(piece);
                break;
            }
            default: invalid();
            }
        }
        invalid();
    }
    Json value(unsigned depth) {
        if (depth > 64) invalid();
        if (take('{')) {
            Json::Object object;
            if (!take('}')) {
                do {
                    auto name = string();
                    need(':');
                    object.insert_or_assign(std::move(name), value(depth + 1));
                } while (take(','));
                need('}');
            }
            return {std::move(object)};
        }
        if (take('[')) {
            Json::Array array;
            if (!take(']')) {
                do { array.push_back(value(depth + 1)); } while (take(','));
                need(']');
            }
            return {std::move(array)};
        }
        if (input.empty()) invalid();
        if (input.front() == '"') return {string()};
        if (input.starts_with("null")) { input.remove_prefix(4); return {}; }
        if (input.starts_with("true")) { input.remove_prefix(4); return {true}; }
        if (input.starts_with("false")) { input.remove_prefix(5); return {false}; }
        double number{};
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr == input.data() || !std::isfinite(number)) invalid();
        input.remove_prefix(static_cast<size_t>(parsed.ptr - input.data()));
        return {number};
    }
public:
    explicit Parser(std::string_view data) : input(data) {}
    Json parse() {
        auto result = value(0);
        if (take('\0') || !input.empty()) invalid();
        return result;
    }
};

}
