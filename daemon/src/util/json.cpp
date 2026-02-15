#include "util/json.h"

#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace khor {

JsonValue JsonValue::make_null() { return JsonValue{}; }

JsonValue JsonValue::make_bool(bool v) {
  JsonValue j;
  j.type = Type::Bool;
  j.b = v;
  return j;
}

JsonValue JsonValue::make_number(double v) {
  JsonValue j;
  j.type = Type::Number;
  j.num = v;
  return j;
}

JsonValue JsonValue::make_string(std::string v) {
  JsonValue j;
  j.type = Type::String;
  j.s = std::move(v);
  return j;
}

JsonValue JsonValue::make_array(std::vector<JsonValue> v) {
  JsonValue j;
  j.type = Type::Array;
  j.a = std::move(v);
  return j;
}

JsonValue JsonValue::make_object(std::map<std::string, JsonValue> v) {
  JsonValue j;
  j.type = Type::Object;
  j.o = std::move(v);
  return j;
}

namespace {

struct Parser {
  std::string_view in;
  size_t i = 0;

  char peek() const { return i < in.size() ? in[i] : '\0'; }
  bool eof() const { return i >= in.size(); }

  void skip_ws() {
    while (!eof()) {
      char c = in[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        i++;
      } else {
        break;
      }
    }
  }

  bool consume(char c) {
    if (peek() != c) return false;
    i++;
    return true;
  }

  [[noreturn]] void fail(const char* msg) {
    throw std::runtime_error(msg);
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
      out.push_back((char)cp);
    } else if (cp <= 0x7FFu) {
      out.push_back((char)(0xC0u | ((cp >> 6) & 0x1Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
      out.push_back((char)(0xE0u | ((cp >> 12) & 0x0Fu)));
      out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back((char)(0xF0u | ((cp >> 18) & 0x07u)));
      out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
  }

  uint32_t parse_hex4() {
    if (i + 4 > in.size()) fail("incomplete \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
      char c = in[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint32_t)(10 + (c - 'a'));
      else if (c >= 'A' && c <= 'F') v |= (uint32_t)(10 + (c - 'A'));
      else fail("invalid hex in \\u escape");
    }
    return v;
  }

  std::string parse_string() {
    if (!consume('"')) fail("expected string");
    std::string out;
    while (!eof()) {
      char c = in[i++];
      if (c == '"') return out;
      if ((unsigned char)c < 0x20) fail("control char in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (eof()) fail("incomplete escape");
      char e = in[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = parse_hex4();
          // Handle surrogate pairs.
          if (cp >= 0xD800u && cp <= 0xDBFFu) {
            // high surrogate, expect low surrogate
            if (i + 6 <= in.size() && in[i] == '\\' && in[i + 1] == 'u') {
              i += 2;
              uint32_t lo = parse_hex4();
              if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
              } else {
                fail("invalid low surrogate");
              }
            } else {
              fail("missing low surrogate");
            }
          }
          append_utf8(out, cp);
          break;
        }
        default:
          fail("invalid escape");
      }
    }
    fail("unterminated string");
  }

  double parse_number() {
    size_t start = i;
    if (peek() == '-') i++;
    if (peek() == '0') {
      i++;
    } else if (std::isdigit((unsigned char)peek())) {
      while (std::isdigit((unsigned char)peek())) i++;
    } else {
      fail("invalid number");
    }

    if (peek() == '.') {
      i++;
      if (!std::isdigit((unsigned char)peek())) fail("invalid number fraction");
      while (std::isdigit((unsigned char)peek())) i++;
    }

    if (peek() == 'e' || peek() == 'E') {
      i++;
      if (peek() == '+' || peek() == '-') i++;
      if (!std::isdigit((unsigned char)peek())) fail("invalid number exponent");
      while (std::isdigit((unsigned char)peek())) i++;
    }

    std::string tmp(in.substr(start, i - start));
    char* endp = nullptr;
    double v = std::strtod(tmp.c_str(), &endp);
    if (!endp || *endp != '\0') fail("invalid number");
    return v;
  }

  void expect_literal(std::string_view lit) {
    if (in.substr(i, lit.size()) != lit) fail("invalid literal");
    i += lit.size();
  }

  JsonValue parse_value() {
    skip_ws();
    char c = peek();
    if (c == 'n') {
      expect_literal("null");
      return JsonValue::make_null();
    }
    if (c == 't') {
      expect_literal("true");
      return JsonValue::make_bool(true);
    }
    if (c == 'f') {
      expect_literal("false");
      return JsonValue::make_bool(false);
    }
    if (c == '"') {
      return JsonValue::make_string(parse_string());
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '{') {
      return parse_object();
    }
    if (c == '-' || std::isdigit((unsigned char)c)) {
      return JsonValue::make_number(parse_number());
    }
    fail("unexpected token");
  }

  JsonValue parse_array() {
    if (!consume('[')) fail("expected [");
    skip_ws();
    std::vector<JsonValue> arr;
    if (consume(']')) return JsonValue::make_array(std::move(arr));
    while (true) {
      arr.push_back(parse_value());
      skip_ws();
      if (consume(']')) break;
      if (!consume(',')) fail("expected , or ]");
    }
    return JsonValue::make_array(std::move(arr));
  }

  JsonValue parse_object() {
    if (!consume('{')) fail("expected {");
    skip_ws();
    std::map<std::string, JsonValue> obj;
    if (consume('}')) return JsonValue::make_object(std::move(obj));
    while (true) {
      skip_ws();
      if (peek() != '"') fail("expected object key string");
      std::string key = parse_string();
      skip_ws();
      if (!consume(':')) fail("expected :");
      JsonValue val = parse_value();
      obj.emplace(std::move(key), std::move(val));
      skip_ws();
      if (consume('}')) break;
      if (!consume(',')) fail("expected , or }");
    }
    return JsonValue::make_object(std::move(obj));
  }
};

static void dump_string(std::ostringstream& oss, const std::string& s) {
  oss << '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          oss << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
        } else {
          oss << (char)c;
        }
    }
  }
  oss << '"';
}

static void dump_value(std::ostringstream& oss, const JsonValue& v, int indent, int depth) {
  auto newline = [&] {
    if (indent > 0) oss << "\n" << std::string((size_t)depth * (size_t)indent, ' ');
  };

  switch (v.type) {
    case JsonValue::Type::Null:
      oss << "null";
      return;
    case JsonValue::Type::Bool:
      oss << (v.b ? "true" : "false");
      return;
    case JsonValue::Type::Number: {
      // Use a stable representation without scientific noise for integers.
      double n = v.num;
      if (std::isfinite(n) && std::floor(n) == n) {
        oss << (int64_t)n;
      } else {
        oss << n;
      }
      return;
    }
    case JsonValue::Type::String:
      dump_string(oss, v.s);
      return;
    case JsonValue::Type::Array: {
      oss << '[';
      if (!v.a.empty()) {
        for (size_t idx = 0; idx < v.a.size(); idx++) {
          if (idx) oss << ',';
          if (indent > 0) {
            oss << "\n" << std::string((size_t)(depth + 1) * (size_t)indent, ' ');
          }
          dump_value(oss, v.a[idx], indent, depth + 1);
        }
        if (indent > 0) {
          oss << "\n" << std::string((size_t)depth * (size_t)indent, ' ');
        }
      }
      oss << ']';
      return;
    }
    case JsonValue::Type::Object: {
      oss << '{';
      if (!v.o.empty()) {
        size_t idx = 0;
        for (const auto& [k, val] : v.o) {
          if (idx++) oss << ',';
          if (indent > 0) {
            oss << "\n" << std::string((size_t)(depth + 1) * (size_t)indent, ' ');
          }
          dump_string(oss, k);
          oss << (indent > 0 ? ": " : ":");
          dump_value(oss, val, indent, depth + 1);
        }
        if (indent > 0) {
          oss << "\n" << std::string((size_t)depth * (size_t)indent, ' ');
        }
      }
      oss << '}';
      return;
    }
  }
}

} // namespace

bool json_parse(std::string_view in, JsonValue* out, JsonParseError* err) {
  if (!out) return false;

  try {
    Parser p{in};
    JsonValue v = p.parse_value();
    p.skip_ws();
    if (!p.eof()) {
      throw std::runtime_error("trailing characters");
    }
    *out = std::move(v);
    return true;
  } catch (const std::exception& e) {
    if (err) {
      err->offset = 0;
      err->message = e.what();
    }
    return false;
  }
}

std::string json_stringify(const JsonValue& v, int indent) {
  std::ostringstream oss;
  dump_value(oss, v, indent, 0);
  if (indent > 0) oss << "\n";
  return oss.str();
}

const JsonValue* json_get(const JsonValue& obj, const char* key) {
  if (!key) return nullptr;
  if (!obj.is_object()) return nullptr;
  auto it = obj.o.find(key);
  if (it == obj.o.end()) return nullptr;
  return &it->second;
}

bool json_get_bool(const JsonValue& obj, const char* key, bool def) {
  const JsonValue* v = json_get(obj, key);
  if (!v) return def;
  if (!v->is_bool()) return def;
  return v->b;
}

double json_get_number(const JsonValue& obj, const char* key, double def) {
  const JsonValue* v = json_get(obj, key);
  if (!v) return def;
  if (!v->is_number()) return def;
  return v->num;
}

std::string json_get_string(const JsonValue& obj, const char* key, const std::string& def) {
  const JsonValue* v = json_get(obj, key);
  if (!v) return def;
  if (!v->is_string()) return def;
  return v->s;
}

} // namespace khor
