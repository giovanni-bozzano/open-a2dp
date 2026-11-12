#pragma once

// MSVC shim for the vendored Sony LDAC encoder.
//
// The AOSP drop is built by clang there and uses GNU attributes the compiler there understands.
// This header is ours, not part of the drop: it is force-included (/FI) into the three vendored
// translation units only, so the sources stay byte-for-byte as Sony contributed them and nobody
// has to keep a patched copy in step with AOSP.

#if defined(_MSC_VER)
#ifndef __attribute__
// `__attribute__((unused))` marks a static helper the code keeps for symmetry with other
// builds. MSVC has no equivalent it needs here, so the attribute becomes nothing.
#define __attribute__(x)
#endif
#endif
