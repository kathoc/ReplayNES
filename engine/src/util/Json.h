// Minimal JSON value / parser / writer (own implementation; no third-party dependency).
// Objects keep insertion order so output is deterministic. Integers are exact int64.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rn {

class Json {
 public:
  enum class Type { Null, Bool, Int, Double, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : t_(Type::Bool), b_(b) {}
  Json(int v) : t_(Type::Int), i_(v) {}
  Json(int64_t v) : t_(Type::Int), i_(v) {}
  Json(uint64_t v) : t_(Type::Int), i_(int64_t(v)) {}
  Json(uint32_t v) : t_(Type::Int), i_(int64_t(v)) {}
  Json(double v) : t_(Type::Double), d_(v) {}
  Json(const char* s) : t_(Type::String), s_(s) {}
  Json(std::string s) : t_(Type::String), s_(std::move(s)) {}

  static Json array() { Json j; j.t_ = Type::Array; return j; }
  static Json object() { Json j; j.t_ = Type::Object; return j; }

  Type type() const { return t_; }
  bool isNull() const { return t_ == Type::Null; }
  bool isObject() const { return t_ == Type::Object; }
  bool isArray() const { return t_ == Type::Array; }
  bool isString() const { return t_ == Type::String; }
  bool isNumber() const { return t_ == Type::Int || t_ == Type::Double; }
  bool isBool() const { return t_ == Type::Bool; }

  bool asBool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
  int64_t asInt(int64_t def = 0) const {
    return t_ == Type::Int ? i_ : t_ == Type::Double ? int64_t(d_) : def;
  }
  double asDouble(double def = 0) const {
    return t_ == Type::Double ? d_ : t_ == Type::Int ? double(i_) : def;
  }
  const std::string& asString() const { static const std::string e; return t_ == Type::String ? s_ : e; }

  // Array
  size_t size() const { return t_ == Type::Array ? a_.size() : t_ == Type::Object ? o_.size() : 0; }
  const Json& at(size_t i) const { static const Json n; return (t_ == Type::Array && i < a_.size()) ? a_[i] : n; }
  void push(Json v) { if (t_ != Type::Array) { *this = array(); } a_.push_back(std::move(v)); }
  const std::vector<Json>& items() const { return a_; }

  // Object
  bool has(const std::string& k) const;
  const Json& operator[](const std::string& k) const;
  void set(const std::string& k, Json v);
  const std::vector<std::pair<std::string, Json>>& members() const { return o_; }

  std::string dump(int indent = 2) const;
  // Returns false and fills err on syntax error.
  static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

 private:
  void dumpTo(std::string& out, int indent, int depth) const;
  Type t_ = Type::Null;
  bool b_ = false;
  int64_t i_ = 0;
  double d_ = 0;
  std::string s_;
  std::vector<Json> a_;
  std::vector<std::pair<std::string, Json>> o_;
};

}  // namespace rn
