// Pauses the programs that render to a sink, so the audio engine releases the sink's A2DP node.
//
// Windows offers no way to end another program's stream: the handle the PnP veto is about
// belongs to the audio engine, and only the rendering program can close its session. The media
// transport manager is the supported way to ask it to stop, and a program that publishes a media
// session - a browser, a music app, the Media app - treats it as its own pause button.

#include "SinkPlayback.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/base.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace OpenA2dp::Tools {
namespace {

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

// The failure as a message can show it: the code is what says whose refusal it was.
std::string Code(const winrt::hresult_error& failure)
{
	char text[16] = {};
	(void)std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(failure.code()));
	return text;
}

std::string LowerAscii(std::string text)
{
	for (char& character : text)
	{
		if (character >= 'A' && character <= 'Z')
		{
			character = static_cast<char>(character - 'A' + 'a');
		}
	}
	return text;
}

// What a message calls the program a media session belongs to: the part of the app id after its
// last separator, which is the image name for a desktop program.
std::string AppTitle(const winrt::hstring& appId)
{
	std::string id = winrt::to_string(appId);
	const size_t bang = id.rfind('!');
	if (bang != std::string::npos)
	{
		id = id.substr(bang + 1);
	}
	const size_t separator = id.find_last_of("\\/");
	if (separator != std::string::npos)
	{
		id = id.substr(separator + 1);
	}
	return id;
}

// True when the session belongs to one of the programs rendering to the sink. The app id holds
// the image name for a desktop program in one form or another ("spotify.exe", its full path, or
// an id that starts with it), so the name without its extension identifies the player.
bool IsPlayer(const winrt::hstring& appId, const std::vector<std::string>& players)
{
	const std::string id = LowerAscii(winrt::to_string(appId));
	constexpr const char* kExe = ".exe";
	for (const std::string& player : players)
	{
		std::string name = LowerAscii(player);
		if (name.size() > 4 && name.compare(name.size() - 4, 4, kExe) == 0)
		{
			name.resize(name.size() - 4);
		}
		if (!name.empty() && id.find(name) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

// The pause itself, on a thread of its own: the window runs a single-threaded apartment and must
// not be blocked while a WinRT operation completes.
void PauseOnWorker(const std::vector<std::string>& players, std::string& paused, std::string& error)
{
	std::thread worker([&players, &paused, &error]() {
		try
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		}
		catch (const winrt::hresult_error&)
		{
			// The thread already carries an apartment; the media calls work from either one.
		}
		try
		{
			const GlobalSystemMediaTransportControlsSessionManager manager =
				GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
			std::vector<GlobalSystemMediaTransportControlsSession> matching;
			std::vector<GlobalSystemMediaTransportControlsSession> playing;
			for (const GlobalSystemMediaTransportControlsSession& session : manager.GetSessions())
			{
				if (session.GetPlaybackInfo().PlaybackStatus() !=
					GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
				{
					continue;
				}
				if (IsPlayer(session.SourceAppUserModelId(), players))
				{
					matching.push_back(session);
				}
				else
				{
					playing.push_back(session);
				}
			}
			// No app id matched a holder of the node, but something is playing: the sink is the
			// output, so that is what has to stop before the node can be released. A program
			// that was not holding the node simply resumes where it was.
			const std::vector<GlobalSystemMediaTransportControlsSession>& targets =
				matching.empty() ? playing : matching;
			for (const GlobalSystemMediaTransportControlsSession& session : targets)
			{
				if (!session.TryPauseAsync().get())
				{
					continue;
				}
				const std::string title = AppTitle(session.SourceAppUserModelId());
				if (title.empty())
				{
					continue;
				}
				paused += paused.empty() ? title : ", " + title;
			}
			if (paused.empty())
			{
				error = "it publishes no media session to pause";
			}
		}
		catch (const winrt::hresult_error& failure)
		{
			error = "its media session could not be read (" + Code(failure) + ")";
		}
		(void)winrt::uninit_apartment();
	});
	worker.join();
}

} // namespace

bool PauseSinkPlayback(const std::vector<std::string>& players, std::string& paused,
					   std::string& error)
{
	paused.clear();
	error.clear();
	PauseOnWorker(players, paused, error);
	if (paused.empty() && error.empty())
	{
		error = "no media session could be paused";
	}
	return !paused.empty();
}

} // namespace OpenA2dp::Tools
