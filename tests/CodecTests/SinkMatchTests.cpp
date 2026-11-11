// The switch decides which sink's driver to move and which programs to pause before it does.
// Both decisions are string matches over text the PnP and media-session APIs hand out, and both
// fail quietly when they are wrong - the wrong node is skipped, or a program is left playing -
// so the matches are checked here rather than only against a paired sink.

#include "TestFramework.h"

#include "common/SinkMatch.h"

#include <string>
#include <vector>

using namespace OpenA2dp::Tools;

TEST(SinkMatch_AddressInInstanceIdReadsTheLastAddress)
{
	CHECK(AddressInInstanceId(L"BTHENUM\\DEV_001122AABBCC\\7&1A2B3C4D&0&001122AABBCC_C00000000") ==
		  "001122AABBCC");
	// The byte-reversed form some builds write is still the sink's address, in its written order.
	CHECK(AddressInInstanceId(L"BTHENUM\\DEV_CCBBAA221100\\7&1A2B3C4D&0&CCBBAA221100_C00000000") ==
		  "CCBBAA221100");
	// Lower case in, upper case out: the same string the control device is named with.
	CHECK(AddressInInstanceId(L"bthnum\\dev_001122aabbcc") == "001122AABBCC");
	// A run that is not exactly 12 hex digits is not an address.
	CHECK(AddressInInstanceId(L"BTHENUM\\DEV_001122AABBC").empty());
	CHECK(AddressInInstanceId(L"BTHENUM\\DEV_0001122AABBCC").empty());
	CHECK(AddressInInstanceId(L"").empty());
}

TEST(SinkMatch_MatchesSinkAddressInEitherByteOrder)
{
	const std::wstring forward = L"BTHENUM\\DEV_001122AABBCC\\7&1A2B3C4D&0&001122AABBCC_C00000000";
	const std::wstring reversed = L"BTHENUM\\DEV_CCBBAA221100\\7&1A2B3C4D&0&CCBBAA221100_C00000000";
	CHECK(MatchesSinkAddress(forward, "001122AABBCC"));
	CHECK(MatchesSinkAddress(reversed, "001122AABBCC"));
	// The user types the address in whatever form the sink reports it.
	CHECK(MatchesSinkAddress(forward, "001122aabbcc"));
	CHECK(MatchesSinkAddress(forward, "00:11:22:AA:BB:CC") == false);
	CHECK(MatchesSinkAddress(forward, "001122AABBCD") == false);
	CHECK(MatchesSinkAddress(forward, "001122AABBC") == false);
	CHECK(MatchesSinkAddress(forward, "") == false);
	// The reversed check must not turn a short reference into a match.
	CHECK(MatchesSinkAddress(reversed, "CCBBAA") == false);
}

TEST(SinkMatch_A2dpServiceIdIsTheSourceServiceAlone)
{
	const std::wstring source = L"{0000110B-0000-1000-8000-00805F9B34FB}";
	CHECK(IsA2dpServiceId(source));
	CHECK(IsA2dpServiceId(L"{0000110b-0000-1000-8000-00805f9b34fb}"));
	// The node also offers these, and each names some other package or describes the sink.
	CHECK(IsA2dpServiceId(source + L"_GENERIC") == false);
	CHECK(IsA2dpServiceId(source + L"_LOCALMFG&0000") == false);
	CHECK(IsA2dpServiceId(L"{0000111E-0000-1000-8000-00805F9B34FB}") == false);
	CHECK(IsA2dpServiceId(L"") == false);
}

TEST(SinkMatch_DriverOfServiceNameMatchesCaseInsensitively)
{
	CHECK(DriverOfServiceName(L"OpenA2dp") == SinkDriver::OpenA2dp);
	CHECK(DriverOfServiceName(L"opena2dp") == SinkDriver::OpenA2dp);
	CHECK(DriverOfServiceName(L"BthA2dp") == SinkDriver::Windows);
	CHECK(DriverOfServiceName(L"BTWAMPFL_btha2dp") == SinkDriver::Windows);
	CHECK(DriverOfServiceName(L"BthEnum") == SinkDriver::Unknown);
	CHECK(DriverOfServiceName(L"") == SinkDriver::Unknown);
}

TEST(SinkMatch_PlayerTitleIsTheImageName)
{
	CHECK(PlayerTitleFromAppId("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify.exe") == "Spotify.exe");
	CHECK(PlayerTitleFromAppId("C:\\Program Files\\App\\Music.exe") == "Music.exe");
	CHECK(PlayerTitleFromAppId("Music.exe") == "Music.exe");
	CHECK(PlayerTitleFromAppId("").empty());
}

TEST(SinkMatch_AppIdMatchesPlayerByImageName)
{
	const std::vector<std::string> players = {"Spotify.exe"};
	CHECK(AppIdMatchesPlayer("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify.exe", players));
	CHECK(AppIdMatchesPlayer("C:\\Program Files\\Spotify\\Spotify.exe", players));
	CHECK(AppIdMatchesPlayer("spotify.exe", players));
	CHECK(AppIdMatchesPlayer("C:\\Program Files\\Other\\Chrome.exe", players) == false);
	CHECK(AppIdMatchesPlayer("Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic", players) == false);
	// A package id carries the family name before the `!`, and that names the player too.
	CHECK(AppIdMatchesPlayer("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Preview.exe", players));
	CHECK(AppIdMatchesPlayer("spotify.exe", std::vector<std::string>()) == false);
	CHECK(AppIdMatchesPlayer("spotify.exe", std::vector<std::string>{""}) == false);
	// Case is not part of the identity on either side.
	CHECK(AppIdMatchesPlayer("spotify.exe", std::vector<std::string>{"SPOTIFY.EXE"}));
	CHECK(AppIdMatchesPlayer("music.exe", std::vector<std::string>{"MyApp.exe", "Music"}));
}
