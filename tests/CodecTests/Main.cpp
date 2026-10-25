#include "TestFramework.h"

#include <windows.h>
#include <objbase.h>

#include <cstring>

int main(int argc, char** argv)
{
	const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const char* filter = argc > 1 ? argv[1] : nullptr;

	int run = 0;
	int failedTests = 0;
	for (const auto& test : TestFramework::Registry()) {
		if (filter && std::strstr(test.name, filter) == nullptr) {
			continue;
		}
		const int before = TestFramework::FailureCount();
		std::printf("[ RUN  ] %s\n", test.name);
		test.body();
		const bool ok = TestFramework::FailureCount() == before;
		std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", test.name);
		++run;
		failedTests += ok ? 0 : 1;
	}

	std::printf("\n%d tests run, %d failed\n", run, failedTests);
	if (SUCCEEDED(hr)) {
		CoUninitialize();
	}
	return failedTests == 0 ? 0 : 1;
}
