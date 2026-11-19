#pragma once

#include "SetupTypes.h"

#include <windows.h>

#include <string>

namespace OpenA2dp::Setup {

struct PackageTrust
{
	std::wstring signer;
	std::wstring thumbprint;
	// True when this run added the signer to the machine Root store, so an uninstall or a failed
	// install knows it may take it back out.
	bool added = false;
};

// Verifies the extracted driver package, asks before trusting an untrusted signer, and refuses a
// test-signed package on a machine that does not accept test signing. Nothing is installed here.
bool PrepareDriverPackage(const std::wstring& packageDir, HWND owner, PackageTrust& trust,
						  const Log& log, std::wstring& error);

// Removes a signer this installer added, by SHA-256 thumbprint. A certificate that is already
// gone is not an error.
bool RemoveTrustedCertificate(const std::wstring& thumbprint, std::wstring& error);

} // namespace OpenA2dp::Setup
