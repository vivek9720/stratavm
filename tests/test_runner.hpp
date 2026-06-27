// test_runner.hpp - minimal, zero-dependency test harness.
#ifndef STRATAVM_TEST_RUNNER_HPP
#define STRATAVM_TEST_RUNNER_HPP

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace stratavm::test {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

inline void register_case(const char* name, std::function<void()> fn) {
  registry().push_back({name, std::move(fn)});
}

inline int run_all(int argc, char** argv) {
  const char* filter = argc >= 2 ? argv[1] : nullptr;
  int passed = 0, failed = 0, skipped = 0;
  for (auto& c : registry()) {
    if (filter && c.name.find(filter) == std::string::npos) { ++skipped; continue; }
    std::printf("[ RUN  ] %s\n", c.name.c_str());
    try {
      c.fn();
      std::printf("[ PASS ] %s\n", c.name.c_str());
      ++passed;
    } catch (const std::exception& e) {
      std::printf("[ FAIL ] %s: %s\n", c.name.c_str(), e.what());
      ++failed;
    }
  }
  std::printf("\nResults: %d passed, %d failed, %d skipped\n", passed, failed, skipped);
  return failed ? 1 : 0;
}

struct Failure : std::exception {
  std::string msg;
  explicit Failure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

#define STEST(name)                                                       \
  static void _test_##name();                                             \
  static const bool _reg_##name = (                                       \
      ::stratavm::test::register_case(#name, _test_##name), true);        \
  static void _test_##name()

#define REQUIRE(cond)                                                     \
  do {                                                                    \
    if (!(cond))                                                          \
      throw ::stratavm::test::Failure(                                    \
          std::string("REQUIRE failed: " #cond " at " __FILE__ ":") +    \
          std::to_string(__LINE__));                                       \
  } while (0)

#define REQUIRE_EQ(a, b)                                                  \
  do {                                                                    \
    auto _a = (a); auto _b = (b);                                         \
    if (!(_a == _b))                                                      \
      throw ::stratavm::test::Failure(                                    \
          std::string("REQUIRE_EQ failed at " __FILE__ ":") +            \
          std::to_string(__LINE__));                                       \
  } while (0)

#define REQUIRE_OK(expr)                                                  \
  do {                                                                    \
    auto _s = (expr);                                                     \
    if (_s.is_error())                                                    \
      throw ::stratavm::test::Failure(                                    \
          std::string("REQUIRE_OK failed: ") + _s.to_string() +          \
          " at " __FILE__ ":" + std::to_string(__LINE__));                \
  } while (0)

#define REQUIRE_ERR(expr, expected_code)                                  \
  do {                                                                    \
    auto _s = (expr);                                                     \
    if (_s.is_ok())                                                       \
      throw ::stratavm::test::Failure(                                    \
          std::string("REQUIRE_ERR: expected error but got ok at ")       \
          + __FILE__ ":" + std::to_string(__LINE__));                     \
    if (_s.code() != (expected_code))                                     \
      throw ::stratavm::test::Failure(                                    \
          std::string("REQUIRE_ERR: wrong code '") + _s.to_string() +    \
          "' at " __FILE__ ":" + std::to_string(__LINE__));               \
  } while (0)

} // namespace stratavm::test

#endif // STRATAVM_TEST_RUNNER_HPP
