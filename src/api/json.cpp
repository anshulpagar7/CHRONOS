#include "chronos/api/json.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace chronos::api {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue run() {
        JsonValue v = parse_value(0);
        skip_ws();
        if (pos_ != text_.size()) {
            fail("trailing characters after JSON value");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("json parse error at offset " +
                                 std::to_string(pos_) + ": " + what);
    }

    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    char peek() {
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
        }
        return text_[pos_];
    }

    char take() {
        const char c = peek();
        ++pos_;
        return c;
    }

    void expect_literal(std::string_view lit) {
        if (text_.substr(pos_, lit.size()) != lit) {
            fail("invalid literal");
        }
        pos_ += lit.size();
    }

    JsonValue parse_value(int depth) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
        }
        skip_ws();
        switch (peek()) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return JsonValue(parse_string());
            case 't': expect_literal("true"); return JsonValue(true);
            case 'f': expect_literal("false"); return JsonValue(false);
            case 'n': expect_literal("null"); return JsonValue(nullptr);
            default:  return parse_number();
        }
    }

    JsonValue parse_object(int depth) {
        take();  // '{'
        JsonValue obj = JsonValue::object();
        skip_ws();
        if (peek() == '}') {
            take();
            return obj;
        }
        while (true) {
            skip_ws();
            if (peek() != '"') {
                fail("expected object key");
            }
            std::string key = parse_string();
            skip_ws();
            if (take() != ':') {
                fail("expected ':' after key");
            }
            obj.set(key, parse_value(depth + 1));
            skip_ws();
            const char c = take();
            if (c == '}') {
                return obj;
            }
            if (c != ',') {
                fail("expected ',' or '}' in object");
            }
        }
    }

    JsonValue parse_array(int depth) {
        take();  // '['
        JsonValue arr = JsonValue::array();
        skip_ws();
        if (peek() == ']') {
            take();
            return arr;
        }
        while (true) {
            arr.push(parse_value(depth + 1));
            skip_ws();
            const char c = take();
            if (c == ']') {
                return arr;
            }
            if (c != ',') {
                fail("expected ',' or ']' in array");
            }
        }
    }

    std::string parse_string() {
        take();  // '"'
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = take();
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("raw control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            const char esc = take();
            switch (esc) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':  append_unicode_escape(out); break;
                default:   fail("invalid escape");
            }
        }
    }

    void append_unicode_escape(std::string& out) {
        if (pos_ + 4 > text_.size()) {
            fail("truncated \\u escape");
        }
        unsigned int code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = take();
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code |= static_cast<unsigned int>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<unsigned int>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<unsigned int>(h - 'A' + 10);
            } else {
                fail("invalid \\u escape");
            }
        }
        // Basic-plane only (documented limit). Encode as UTF-8.
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            take();
        }
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0 ||
                text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E' ||
                text_[pos_] == '+' || text_[pos_] == '-')) {
            ++pos_;
        }
        double value = 0;
        const auto [ptr, ec] =
            std::from_chars(text_.data() + start, text_.data() + pos_, value);
        if (ec != std::errc{} || ptr != text_.data() + pos_ || pos_ == start) {
            fail("invalid number");
        }
        return JsonValue(value);
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

void escape_into(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void format_number_into(std::string& out, double d) {
    if (std::floor(d) == d && std::abs(d) < 9.007199254740992e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        out += buf;
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.10g", d);
        out += buf;
    }
}

}  // namespace

JsonValue JsonValue::parse(std::string_view text) {
    return Parser(text).run();
}

JsonValue& JsonValue::set(const std::string& key, JsonValue v) {
    std::get<Object>(value_)[key] = std::move(v);
    return *this;
}

bool JsonValue::contains(const std::string& key) const {
    if (!is_object()) {
        return false;
    }
    return as_object().count(key) > 0;
}

const JsonValue& JsonValue::at(const std::string& key) const {
    static const JsonValue null_value;
    if (!is_object()) {
        return null_value;
    }
    const auto it = as_object().find(key);
    return it == as_object().end() ? null_value : it->second;
}

JsonValue& JsonValue::push(JsonValue v) {
    std::get<Array>(value_).push_back(std::move(v));
    return *this;
}

std::size_t JsonValue::size() const {
    if (is_array()) {
        return as_array().size();
    }
    if (is_object()) {
        return as_object().size();
    }
    return 0;
}

std::string JsonValue::dump() const {
    std::string out;
    struct Visitor {
        std::string& out;
        void operator()(std::nullptr_t) const { out += "null"; }
        void operator()(bool b) const { out += b ? "true" : "false"; }
        void operator()(double d) const { format_number_into(out, d); }
        void operator()(const std::string& s) const { escape_into(out, s); }
        void operator()(const Array& a) const {
            out += '[';
            bool first = true;
            for (const JsonValue& v : a) {
                if (!first) {
                    out += ',';
                }
                first = false;
                out += v.dump();
            }
            out += ']';
        }
        void operator()(const Object& o) const {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : o) {
                if (!first) {
                    out += ',';
                }
                first = false;
                escape_into(out, k);
                out += ':';
                out += v.dump();
            }
            out += '}';
        }
    };
    std::visit(Visitor{out}, value_);
    return out;
}

}  // namespace chronos::api
