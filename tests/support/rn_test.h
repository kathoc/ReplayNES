// Minimal doctest-compatible test macros (TEST_CASE, CHECK, REQUIRE, CHECK_EQ, REQUIRE_EQ,
// CHECK_FALSE, MESSAGE, FAIL). Own implementation so the repo needs no downloaded third-party
// header; tests can be moved to real doctest by swapping this include.
#pragma once
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>

namespace rntest {
using Fn = void (*)();
struct Registrar {
  Registrar(const char* name, Fn fn, const char* file, int line);
};
struct RequireFailure {};
void reportFailure(const char* file, int line, const std::string& what);
void message(const char* file, int line, const std::string& what);

template <typename T>
std::string show(const T& v) {
  std::ostringstream os;
  if constexpr (std::is_same<T, bool>::value) os << (v ? "true" : "false");
  else if constexpr (std::is_integral<T>::value || std::is_enum<T>::value) os << static_cast<long long>(v);
  else if constexpr (std::is_floating_point<T>::value) os << v;
  else if constexpr (std::is_convertible<T, std::string>::value) os << '"' << std::string(v) << '"';
  else os << "{?}";
  return os.str();
}
}  // namespace rntest

#define RNT_CAT2(a, b) a##b
#define RNT_CAT(a, b) RNT_CAT2(a, b)
#define TEST_CASE(name)                                                                             \
  static void RNT_CAT(rnt_case_, __LINE__)();                                                       \
  static ::rntest::Registrar RNT_CAT(rnt_reg_, __LINE__)(name, &RNT_CAT(rnt_case_, __LINE__), __FILE__, \
                                                         __LINE__);                                 \
  static void RNT_CAT(rnt_case_, __LINE__)()

#define RNT_CHECK_IMPL(cond, text, fatal)                                    \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ::rntest::reportFailure(__FILE__, __LINE__, text);                     \
      if (fatal) throw ::rntest::RequireFailure();                           \
    }                                                                        \
  } while (0)
#define CHECK(expr) RNT_CHECK_IMPL(static_cast<bool>(expr), "CHECK( " #expr " )", false)
#define REQUIRE(expr) RNT_CHECK_IMPL(static_cast<bool>(expr), "REQUIRE( " #expr " )", true)
#define CHECK_FALSE(expr) RNT_CHECK_IMPL(!static_cast<bool>(expr), "CHECK_FALSE( " #expr " )", false)
#define RNT_EQ_IMPL(a, b, name, fatal)                                                                  \
  do {                                                                                                  \
    const auto& rnt_a = (a);                                                                            \
    const auto& rnt_b = (b);                                                                            \
    if (!(rnt_a == rnt_b)) {                                                                            \
      ::rntest::reportFailure(__FILE__, __LINE__,                                                       \
                              std::string(name "( " #a ", " #b " ) values: ") + ::rntest::show(rnt_a) + \
                                  " != " + ::rntest::show(rnt_b));                                      \
      if (fatal) throw ::rntest::RequireFailure();                                                      \
    }                                                                                                   \
  } while (0)
#define CHECK_EQ(a, b) RNT_EQ_IMPL(a, b, "CHECK_EQ", false)
#define REQUIRE_EQ(a, b) RNT_EQ_IMPL(a, b, "REQUIRE_EQ", true)
#define MESSAGE(x)                                   \
  do {                                               \
    std::ostringstream rnt_os;                       \
    rnt_os << x;                                     \
    ::rntest::message(__FILE__, __LINE__, rnt_os.str()); \
  } while (0)
#define FAIL(x)                                            \
  do {                                                     \
    std::ostringstream rnt_os;                             \
    rnt_os << x;                                           \
    ::rntest::reportFailure(__FILE__, __LINE__, rnt_os.str()); \
    throw ::rntest::RequireFailure();                      \
  } while (0)
