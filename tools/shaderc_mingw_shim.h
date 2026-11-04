// What the MinGW build of bgfx's shaderc needs on top of the WSL stub headers it is generated with.
//
// shaderc_hlsl.cpp defines COM_NO_WINDOWS_H, so the DirectX-Headers' d3dcommon.h has no IUnknown: forcing their own
// wsl/winadapter.h in (which the Linux builds include) gives it one. Its one call into the loader API -- GetModuleFileNameA,
// for the verbose log's "Loaded d3dcompiler" line -- has no declaration there, and the real libloaderapi.h cannot be
// included beside the stubs (their types conflict), so it is declared here. The call links against kernel32 as usual.
//
// Used by tools/build_shaders.py --d3d, which builds a MinGW shaderc: see docs/BUILD_WINDOWS.md.
#pragma once

#include <wsl/winadapter.h>

#ifdef __cplusplus
extern "C"
{
#endif

// stdcall spelled as an attribute: the stub headers define __stdcall away, so the usual keyword would give a cdecl
// reference that kernel32's import library has no symbol for
unsigned long __attribute__((stdcall)) GetModuleFileNameA(void* module, char* name, unsigned long size);

#ifdef __cplusplus
}
#endif
