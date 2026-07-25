#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::api {

/// A small, strict JSON value: parse, build, dump. Covers everything the
/// CHRONOS API needs (objects, arrays, strings, doubles, bools, null) in
/// ~250 lines instead of a vendored dependency (ADR-010).
///
/// Limits, by design: numbers are IEEE doubles (fine for ids < 2^53 and
/// every quantity we serve); \uXXXX escapes decode basic-plane code points
/// only (no surrogate pairs); parse depth is capped to keep hostile input
/// from recursing the stack away.
class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;  // Ordered: stable dumps.

    JsonValue() : value_(nullptr) {}
    JsonValue(std::nullptr_t) : value_(nullptr) {}
    JsonValue(bool b) : value_(b) {}
    JsonValue(double d) : value_(d) {}
    JsonValue(int i) : value_(static_cast<double>(i)) {}
    JsonValue(std::int64_t i) : value_(static_cast<double>(i)) {}
    JsonValue(std::uint64_t u) : value_(static_cast<double>(u)) {}
    JsonValue(const char* s) : value_(std::string(s)) {}
    JsonValue(std::string s) : value_(std::move(s)) {}

    static JsonValue object() { return JsonValue(Object{}); }
    static JsonValue array() { return JsonValue(Array{}); }

    /// Parse strict JSON. Throws std::runtime_error with a position on
    /// malformed input.
    static JsonValue parse(std::string_view text);

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(value_); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(value_); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(value_); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(value_); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(value_); }

    [[nodiscard]] bool as_bool() const { return std::get<bool>(value_); }
    [[nodiscard]] double as_number() const { return std::get<double>(value_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(value_); }
    [[nodiscard]] const Array& as_array() const { return std::get<Array>(value_); }
    [[nodiscard]] const Object& as_object() const { return std::get<Object>(value_); }

    /// Object helpers. set() returns *this for chaining.
    JsonValue& set(const std::string& key, JsonValue v);
    [[nodiscard]] bool contains(const std::string& key) const;
    /// Value for `key`, or null JsonValue if absent / not an object.
    [[nodiscard]] const JsonValue& at(const std::string& key) const;

    /// Array helper.
    JsonValue& push(JsonValue v);

    [[nodiscard]] std::size_t size() const;

    /// Compact serialization (no whitespace), correct string escaping.
    [[nodiscard]] std::string dump() const;

private:
    explicit JsonValue(Object o) : value_(std::move(o)) {}
    explicit JsonValue(Array a) : value_(std::move(a)) {}

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value_;
};

}  // namespace chronos::api
