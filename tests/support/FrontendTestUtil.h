// Helpers for the frontend core tests (C API: replaynes/frontend.h).
#pragma once
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "replaynes/frontend.h"
#include "support/TestUtil.h"

namespace rnft {

// Takes ownership of a heap string returned by the frontend core.
inline std::string take(char* s) {
  std::string out = s ? s : "";
  rnf_string_free(s);
  return out;
}

inline bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

using Pairs = std::vector<std::pair<std::string, std::string>>;

inline Pairs listPairs(rnf_list* l) {
  Pairs out;
  for (size_t i = 0; i < rnf_list_count(l); ++i) out.emplace_back(rnf_list_a(l, i), rnf_list_b(l, i));
  rnf_list_free(l);
  return out;
}

inline std::set<std::string> asSet(const Pairs& p) {
  std::set<std::string> s;
  for (auto& e : p) s.insert(e.first + "=" + e.second);
  return s;
}

// Binding table view over owned strings.
struct Bindings {
  Pairs pairs;
  std::vector<rnf_binding> view() const {
    std::vector<rnf_binding> v;
    for (auto& p : pairs) v.push_back({p.first.c_str(), p.second.c_str()});
    return v;
  }
};

inline Pairs defaultBindings(rnf_keyboard_scheme scheme = RNF_KEYBOARD_MACOS) {
  Pairs out;
  for (size_t i = 0; i < rnf_input_default_binding_count(scheme); ++i) {
    rnf_binding b;
    rnf_input_default_binding_get(scheme, i, &b);
    out.emplace_back(b.input, b.action);
  }
  return out;
}

// Parsed defaults (through the engine JSON, like a fresh bindings file).
inline Pairs defaultConfig() {
  rnf_input_config* c = nullptr;
  char* json = rnf_input_default_config_json(RNF_KEYBOARD_MACOS);
  rnf_input_config_parse(json, &c);
  rnf_string_free(json);
  Pairs out;
  for (size_t i = 0; i < rnf_input_config_binding_count(c); ++i) {
    rnf_binding b;
    rnf_input_config_binding_get(c, i, &b);
    out.emplace_back(b.input, b.action);
  }
  rnf_input_config_free(c);
  return out;
}

struct Plan {
  Pairs unbind, bind;
};

// Applies a plan to a binding table the way rn_input does (unbind exact pairs, then append).
inline Pairs applying(const Plan& plan, Pairs c) {
  for (auto& u : plan.unbind) {
    for (auto it = c.begin(); it != c.end();) {
      if (*it == u) it = c.erase(it); else ++it;
    }
  }
  c.insert(c.end(), plan.bind.begin(), plan.bind.end());
  return c;
}

inline void removeIf(Pairs& c, bool (*pred)(const std::pair<std::string, std::string>&)) {
  for (auto it = c.begin(); it != c.end();) {
    if (pred(*it)) it = c.erase(it); else ++it;
  }
}

}  // namespace rnft
