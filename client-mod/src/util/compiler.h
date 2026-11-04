#pragma once
// Compiler-specific spellings the mod needs in one place, so both MinGW and MSVC build it.
#if defined(_MSC_VER)
#include <intrin.h>
// The return address of the current function (needs no frame pointer).
#define FO_RETURN_ADDRESS() _ReturnAddress()
// Where the current frame's return address sits on the stack: a stack scan starts here.
#define FO_FRAME_ADDRESS() _AddressOfReturnAddress()
#else
#define FO_RETURN_ADDRESS() __builtin_return_address(0)
#define FO_FRAME_ADDRESS() __builtin_frame_address(0)
#endif
