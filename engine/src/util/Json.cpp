#include "util/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rn {

bool Json::has(const std::string& k) const {
  if (t_ != Type::Object) return false;
  for (auto& kv : o_) if (kv.first == k) return true;
  return false;
}

const Json& Json::operator[](const std::string& k) const {
  static const Json n;
  if (t_ != Type::Object) return n;
  for (auto& kv : o_) if (kv.first == k) return kv.second;
  return n;
}

void Json::set(const std::string& k, Json v) {
  if (t_ != Type::Object) *this = object();
  for (auto& kv : o_) if (kv.first == k) { kv.second = std::move(v); return; }
  o_.emplace_back(k, std::move(v));
}

static void escapeTo(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out.push_back(char(c));
    }
  }
  out.push_back('"');
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
  auto nl = [&](int d) {
    if (indent <= 0) return;
    out.push_back('\n');
    out.append(size_t(indent * d), ' ');
  };
  switch (t_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Int: out += std::to_string(i_); break;
    case Type::Double: {
      if (!std::isfinite(d_)) { out += "null"; break; }
      char b[64]; std::snprintf(b, sizeof b, "%.17g", d_); out += b;
      if (!std::strpbrk(b, ".eE")) out += ".0";
      break;
    }
    case Type::String: escapeTo(out, s_); break;
    case Type::Array:
      out.push_back('[');
      for (size_t i = 0; i < a_.size(); ++i) {
        if (i) out.push_back(',');
        nl(depth + 1);
        a_[i].dumpTo(out, indent, depth + 1);
      }
      if (!a_.empty()) nl(depth);
      out.push_back(']');
      break;
    case Type::Object:
      out.push_back('{');
      for (size_t i = 0; i < o_.size(); ++i) {
        if (i) out.push_back(',');
        nl(depth + 1);
        escapeTo(out, o_[i].first);
        out += indent > 0 ? ": " : ":";
        o_[i].second.dumpTo(out, indent, depth + 1);
      }
      if (!o_.empty()) nl(depth);
      out.push_back('}');
      break;
  }
}

std::string Json::dump(int indent) const {
  std::string s;
  dumpTo(s, indent, 0);
  if (indent > 0) s.push_back('\n');
  return s;
}

namespace {
struct Parser {
  const char* p;
  const char* end;
  const char* begin;
  std::string err;
  int depth = 0;

  void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at offset " + std::to_string(p - begin);
    return false;
  }
  bool lit(const char* s) {
    size_t n = std::strlen(s);
    if (size_t(end - p) < n || std::memcmp(p, s, n) != 0) return fail("bad literal");
    p += n;
    return true;
  }
  static void utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o.push_back(char(cp));
    else if (cp < 0x800) { o.push_back(char(0xC0 | cp >> 6)); o.push_back(char(0x80 | (cp & 63))); }
    else if (cp < 0x10000) {
      o.push_back(char(0xE0 | cp >> 12)); o.push_back(char(0x80 | (cp >> 6 & 63))); o.push_back(char(0x80 | (cp & 63)));
    } else {
      o.push_back(char(0xF0 | cp >> 18)); o.push_back(char(0x80 | (cp >> 12 & 63)));
      o.push_back(char(0x80 | (cp >> 6 & 63))); o.push_back(char(0x80 | (cp & 63)));
    }
  }
  bool hex4(uint32_t& v) {
    if (end - p < 4) return fail("bad \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return fail("bad hex digit");
    }
    return true;
  }
  bool str(std::string& o) {
    if (p >= end || *p != '"') return fail("expected string");
    ++p;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c == '\\') {
        if (p >= end) return fail("bad escape");
        char e = *p++;
        switch (e) {
          case '"': o.push_back('"'); break;
          case '\\': o.push_back('\\'); break;
          case '/': o.push_back('/'); break;
          case 'b': o.push_back('\b'); break;
          case 'f': o.push_back('\f'); break;
          case 'n': o.push_back('\n'); break;
          case 'r': o.push_back('\r'); break;
          case 't': o.push_back('\t'); break;
          case 'u': {
            uint32_t cp;
            if (!hex4(cp)) return false;
            if (cp >= 0xD800 && cp < 0xDC00) {
              uint32_t lo;
              if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return fail("lone surrogate");
              p += 2;
              if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(o, cp);
            break;
          }
          default: return fail("bad escape");
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return fail("control char in string");
      } else {
        o.push_back(c);
      }
    }
    if (p >= end) return fail("unterminated string");
    ++p;
    return true;
  }
  bool num(Json& out) {
    const char* s = p;
    bool isFloat = false;
    if (p < end && *p == '-') ++p;
    if (p >= end || !(*p >= '0' && *p <= '9')) return fail("bad number");
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
      if (*p == '.' || *p == 'e' || *p == 'E') isFloat = true;
      ++p;
    }
    std::string t(s, p);
    char* e = nullptr;
    if (!isFloat) {
      long long v = std::strtoll(t.c_str(), &e, 10);
      if (*e) return fail("bad integer");
      out = Json(int64_t(v));
    } else {
      double v = std::strtod(t.c_str(), &e);
      if (*e) return fail("bad float");
      out = Json(v);
    }
    return true;
  }
  bool value(Json& out) {
    if (++depth > 256) return fail("nesting too deep");
    ws();
    if (p >= end) return fail("unexpected end");
    bool ok = true;
    switch (*p) {
      case 'n': ok = lit("null"); out = Json(); break;
      case 't': ok = lit("true"); out = Json(true); break;
      case 'f': ok = lit("false"); out = Json(false); break;
      case '"': { std::string s; ok = str(s); out = Json(std::move(s)); break; }
      case '[': {
        ++p; out = Json::array(); ws();
        if (p < end && *p == ']') { ++p; break; }
        for (;;) {
          Json v;
          if (!value(v)) return false;
          out.push(std::move(v));
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == ']') { ++p; break; }
          return fail("expected , or ]");
        }
        break;
      }
      case '{': {
        ++p; out = Json::object(); ws();
        if (p < end && *p == '}') { ++p; break; }
        for (;;) {
          ws();
          std::string k;
          if (!str(k)) return false;
          ws();
          if (p >= end || *p != ':') return fail("expected :");
          ++p;
          Json v;
          if (!value(v)) return false;
          out.set(k, std::move(v));
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == '}') { ++p; break; }
          return fail("expected , or }");
        }
        break;
      }
      default: ok = num(out);
    }
    --depth;
    return ok;
  }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
  Parser ps{text.data(), text.data() + text.size(), text.data(), {}};
  Json v;
  bool ok = ps.value(v);
  if (ok) {
    ps.ws();
    if (ps.p != ps.end) ok = ps.fail("trailing characters");
  }
  if (!ok) {
    if (err) *err = ps.err;
    return false;
  }
  out = std::move(v);
  return true;
}

}  // namespace rn
