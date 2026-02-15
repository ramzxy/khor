#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace khor {

struct JsonValue {
  enum class Type {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
  };

  Type type = Type::Null;
  bool b = false;
  double num = 0.0;
  std::string s;
  std::vector<JsonValue> a;
  std::map<std::string, JsonValue> o;

  static JsonValue make_null();
  static JsonValue make_bool(bool v);
  static JsonValue make_number(double v);
  static JsonValue make_string(std::string v);
  static JsonValue make_array(std::vector<JsonValue> v);
  static JsonValue make_object(std::map<std::string, JsonValue> v);

  bool is_null() const { return type == Type::Null; }
  bool is_bool() const { return type == Type::Bool; }
  bool is_number() const { return type == Type::Number; }
  bool is_string() const { return type == Type::String; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }
};

struct JsonParseError {
  size_t offset = 0;
  std::string message;
};

bool json_parse(std::string_view in, JsonValue* out, JsonParseError* err);
std::string json_stringify(const JsonValue& v, int indent = 0);

const JsonValue* json_get(const JsonValue& obj, const char* key);

bool json_get_bool(const JsonValue& obj, const char* key, bool def);
double json_get_number(const JsonValue& obj, const char* key, double def);
std::string json_get_string(const JsonValue& obj, const char* key, const std::string& def);

} // namespace khor
