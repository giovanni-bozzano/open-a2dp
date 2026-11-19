#pragma once

// Parsing the driver-store listing the installer reads.
//
// `pnputil /enum-drivers` labels every field in the language of the machine, but the two values
// that identify a package - its published name (`oemNN.inf`) and its original name
// (`opena2dp.inf`) - are not translated. Parsing by value instead of by label is what keeps the
// uninstaller finding every package on a non-English Windows.

#include <string>
#include <vector>

namespace OpenA2dp::Tools {

// Every published name whose block names `opena2dp.inf`, in the order they appear. The input is
// the raw stdout of `pnputil /enum-drivers`; CRLF and blank-line-separated blocks are handled.
std::vector<std::wstring> ParseOpenA2dpPackages(const std::string& output);

} // namespace OpenA2dp::Tools
