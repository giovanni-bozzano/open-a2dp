#pragma once

// `ldacBT_internal.h` includes this header for the AOSP build. The encoder calls nothing from
// it - it does no I/O and no process work - so on Windows the include is satisfied by an empty
// header placed ahead of the system include path for the vendored sources only.
