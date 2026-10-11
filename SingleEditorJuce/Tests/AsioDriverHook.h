#pragma once
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

namespace singleeditor::tests
{
// Linked only by the dedicated driver-characterization executable. This is a
// compile-time call seam, not a registry change or a process-wide COM hook.
HRESULT WINAPI createDriverInstance(REFCLSID, IUnknown*, DWORD, REFIID, void**);
}
