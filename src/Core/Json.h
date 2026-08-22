#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace aiie::json {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), num_(d) {}
    Value(int i) : type_(Type::Number), num_(static_cast<double>(i)) {}
    Value(unsigned u) : type_(Type::Number), num_(static_cast<double>(u)) {}
    Value(int64_t i) : type_(Type::Number), num_(static_cast<double>(i)) {}
    Value(std::wstring s) : type_(Type::String), str_(std::move(s)) {}
    Value(const wchar_t* s) : type_(Type::String), str_(s ? s : L"") {}

    static Value MakeArray() { Value v; v.type_ = Type::Array; return v; }
    static Value MakeObject() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double as_number(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    int64_t as_int(int64_t def = 0) const { return type_ == Type::Number ? static_cast<int64_t>(num_) : def; }
    const std::wstring& as_string(const std::wstring& def = {}) const {
        return type_ == Type::String ? str_ : def;
    }

    const std::vector<Value>& as_array() const { return array_; }
    const std::map<std::wstring, Value>& as_object() const { return object_; }

    std::vector<Value>& array() { return array_; }
    std::map<std::wstring, Value>& object() { return object_; }

    void push_back(Value v) { array_.push_back(std::move(v)); }
    void set(std::wstring key, Value v) { object_.insert_or_assign(std::move(key), std::move(v)); }

    const Value* find(const std::wstring& key) const {
        auto it = object_.find(key);
        return it == object_.end() ? nullptr : &it->second;
    }
    void erase(const std::wstring& key) { object_.erase(key); }
    const Value& at(const std::wstring& key) const { return object_.at(key); }

    // Convenience accessors with defaults (never throw).
    std::wstring get_string(const std::wstring& key, const std::wstring& def = {}) const {
        const Value* v = find(key);
        return v ? v->as_string(def) : def;
    }
    bool get_bool(const std::wstring& key, bool def = false) const {
        const Value* v = find(key);
        return v ? v->as_bool(def) : def;
    }
    int64_t get_int(const std::wstring& key, int64_t def = 0) const {
        const Value* v = find(key);
        return v ? v->as_int(def) : def;
    }
    double get_number(const std::wstring& key, double def = 0.0) const {
        const Value* v = find(key);
        return v ? v->as_number(def) : def;
    }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::wstring str_;
    std::vector<Value> array_;
    std::map<std::wstring, Value> object_;
};

// Throws JsonError on malformed input.
Value Parse(std::wstring_view text);
// Serialize; indent < 0 produces compact output.
std::wstring Serialize(const Value& v, int indent = 2);
// Escape a string for embedding in JSON output.
std::wstring EscapeString(std::wstring_view s);

} // namespace aiie::json