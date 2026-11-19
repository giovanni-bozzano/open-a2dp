// The uninstaller finds the packages to remove by parsing `pnputil /enum-drivers`. Its labels
// are localized, so the parser matches the two values that are not: the published oemNN.inf name
// and the original opena2dp.inf name. A mistake here either leaves a package in the driver store
// or removes a package that is not ours, so the parsing is checked without a driver store.

#include "TestFramework.h"

#include "common/SetupEnumeration.h"

#include <string>
#include <vector>

using namespace OpenA2dp::Tools;

TEST(SetupEnumeration_FindsOpenA2dpPackagesByValue)
{
	const std::string output =
		"Microsoft PnP Utility\r\n\r\n"
		"Published Name:     oem12.inf\r\n"
		"Original Name:      opena2dp.inf\r\n"
		"Provider Name:      OpenA2DP\r\n"
		"Class Name:         MEDIA\r\n"
		"\r\n"
		"Published Name:     oem13.inf\r\n"
		"Original Name:      bcmnfcusb.inf\r\n"
		"Provider Name:      Broadcom\r\n"
		"\r\n"
		"Published Name:     oem99.inf\r\n"
		"Original Name:      opena2dp.inf\r\n"
		"Provider Name:      OpenA2DP\r\n";

	const std::vector<std::wstring> packages = ParseOpenA2dpPackages(output);
	CHECK(packages.size() == 2);
	CHECK(packages[0] == L"oem12.inf");
	CHECK(packages[1] == L"oem99.inf");
}

TEST(SetupEnumeration_IgnoresLabelsAndLookalikes)
{
	// The labels are whatever the machine's language prints; the values are the contract. A name
	// that merely contains opena2dp.inf is not the package, and neither is a driver-store folder.
	const std::string output =
		"Localized Publish Label: oem4.inf\r\n"
		"Localized Original Label: opena2dp.inf\r\n"
		"\r\n"
		"Published Name: oem5.inf\r\n"
		"Original Name: notopena2dp.inf\r\n"
		"\r\n"
		"Published Name: oem6.inf\r\n"
		"Original Name: other.inf\r\n"
		"Store Path: opena2dp.inf_amd64_0123456789abcdef\r\n";

	const std::vector<std::wstring> packages = ParseOpenA2dpPackages(output);
	CHECK(packages.size() == 1);
	CHECK(packages[0] == L"oem4.inf");
}

TEST(SetupEnumeration_HandlesMissingBlankLinesAndDuplicates)
{
	const std::string output =
		"Published Name: oem7.inf\n"
		"Original Name: opena2dp.inf\n"
		"Published Name: oem8.inf\n"
		"Original Name: opena2dp.inf\n"
		"\n"
		"Published Name: oem7.inf\n"
		"Original Name: opena2dp.inf\n";

	const std::vector<std::wstring> packages = ParseOpenA2dpPackages(output);
	CHECK(packages.size() == 2);
	CHECK(packages[0] == L"oem7.inf");
	CHECK(packages[1] == L"oem8.inf");
}

TEST(SetupEnumeration_NoPackages)
{
	CHECK(ParseOpenA2dpPackages("").empty());
	CHECK(ParseOpenA2dpPackages("Microsoft PnP Utility\r\n\r\n").empty());
	CHECK(ParseOpenA2dpPackages("Published Name: oem1.inf\r\nOriginal Name: other.inf\r\n").empty());
	// A published name alone is not a package; the block must name ours.
	CHECK(ParseOpenA2dpPackages("Published Name: oem1.inf\r\n").empty());
}
