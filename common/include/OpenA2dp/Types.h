#pragma once

// Fixed-width integer types for code shared between user mode and the kernel driver.
// The user-mode <stdint.h> conflicts with the WDK kernel headers.

#if defined(_KERNEL_MODE) || defined(_WDMDDK_) || defined(_NTDDK_)
#include <ntdef.h>
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
#else
#include <stddef.h>
#include <stdint.h>
#endif
