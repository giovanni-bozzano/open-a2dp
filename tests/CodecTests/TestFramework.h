#pragma once

// Minimal self-contained test framework (no external dependencies).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace TestFramework {

struct TestCase {
	const char* name;
	std::function<void()> body;
};

inline std::vector<TestCase>& Registry()
{
	static std::vector<TestCase> tests;
	return tests;
}

inline int& FailureCount()
{
	static int failures = 0;
	return failures;
}

struct Registrar {
	Registrar(const char* name, std::function<void()> body) { Registry().push_back({name, std::move(body)}); }
};

inline void ReportFailure(const char* file, int line, const std::string& message)
{
	++FailureCount();
	std::printf("    FAILED %s(%d): %s\n", file, line, message.c_str());
}

} // namespace TestFramework

#define TF_CONCAT_INNER(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT_INNER(a, b)

#define TEST(name)                                                                              \
	static void name();                                                                         \
	static TestFramework::Registrar TF_CONCAT(registrar_, name)(#name, name);                   \
	static void name()

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			TestFramework::ReportFailure(__FILE__, __LINE__, "CHECK(" #cond ")");                \
		}                                                                                       \
	} while (0)

#define CHECK_EQ(a, b)                                                                          \
	do {                                                                                        \
		const auto tfA = (a);                                                                   \
		const auto tfB = (b);                                                                   \
		if (!(tfA == tfB)) {                                                                    \
			TestFramework::ReportFailure(__FILE__, __LINE__,                                    \
				std::string("CHECK_EQ(" #a ", " #b ") got ") + std::to_string(tfA) + " vs " +   \
				std::to_string(tfB));                                                           \
		}                                                                                       \
	} while (0)

#define REQUIRE(cond)                                                                           \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			TestFramework::ReportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");              \
			return;                                                                             \
		}                                                                                       \
	} while (0)
