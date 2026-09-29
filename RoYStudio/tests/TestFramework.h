#pragma once
// Minimal self-contained test framework for RoY Studio.
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace roytest {

struct TestCase {
    const char* name;
    const char* suite;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) { registry().push_back({name, suite, std::move(fn)}); }
};

struct Failure {
    std::string message;
};

void recordCheck(bool ok, const std::string& expr, const char* file, int line);
int checksFailedInCurrentTest();

// Realtime allocation guard: counts heap allocations on the current thread
// while active (global operator new is overridden in the test runner).
extern thread_local bool g_countAllocations;
extern thread_local long g_allocationCount;
struct AllocationCounter {
    AllocationCounter() { g_allocationCount = 0; g_countAllocations = true; }
    ~AllocationCounter() { g_countAllocations = false; }
    long count() const { return g_allocationCount; }
};

// A scratch directory unique to the running test (removed at start of test).
std::filesystem::path tempDir(const std::string& name);

// Test resources (test plugins, plugin host, MOCK engines): the build-tree path baked in by CMake,
// or - when the test package was copied to another machine (native Windows validation) - the
// same name next to the roy_tests executable (`relative`, or the file name of `buildPath`).
std::string testPath(const char* buildPath, const char* relative = nullptr);

} // namespace roytest

#define ROY_TEST_PLUGIN_DIR ::roytest::testPath(ROY_TEST_PLUGIN_DIR_BUILD, "test_plugins")
#define ROY_PLUGIN_HOST_EXE ::roytest::testPath(ROY_PLUGIN_HOST_EXE_BUILD)
#define ROY_CLI_EXE ::roytest::testPath(ROY_CLI_EXE_BUILD)
#define ROY_TEST_STEM_ENGINE ::roytest::testPath(ROY_TEST_STEM_ENGINE_BUILD, "test_plugins")

#define ROY_CAT2(a, b) a##b
#define ROY_CAT(a, b) ROY_CAT2(a, b)
#define TEST_CASE(suite, name)                                                                 \
    static void ROY_CAT(roy_test_fn_, __LINE__)();                                              \
    static roytest::Registrar ROY_CAT(roy_test_reg_, __LINE__)(suite, name, &ROY_CAT(roy_test_fn_, __LINE__)); \
    static void ROY_CAT(roy_test_fn_, __LINE__)()

#define CHECK(expr) roytest::recordCheck(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define REQUIRE(expr)                                                                          \
    do {                                                                                       \
        const bool roy_ok_ = static_cast<bool>(expr);                                          \
        roytest::recordCheck(roy_ok_, #expr, __FILE__, __LINE__);                              \
        if (!roy_ok_) throw roytest::Failure{"REQUIRE failed: " #expr};                        \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                  \
    do {                                                                                       \
        const double roy_a_ = static_cast<double>(a), roy_b_ = static_cast<double>(b);         \
        const bool roy_ok_ = std::fabs(roy_a_ - roy_b_) <= static_cast<double>(tol);           \
        roytest::recordCheck(roy_ok_, std::format("{} ~= {} ({} vs {}, tol {})", #a, #b, roy_a_, roy_b_, static_cast<double>(tol)), __FILE__, __LINE__); \
    } while (0)
#define REQUIRE_MSG_OK(expr, msg)                                                              \
    do {                                                                                       \
        const bool roy_ok_ = static_cast<bool>(expr);                                          \
        roytest::recordCheck(roy_ok_, std::string(#expr) + " :: " + (msg), __FILE__, __LINE__); \
        if (!roy_ok_) throw roytest::Failure{"REQUIRE failed: " #expr};                        \
    } while (0)
#define CHECK_MSG(expr, msg) roytest::recordCheck(static_cast<bool>(expr), std::string(#expr) + " :: " + (msg), __FILE__, __LINE__)
