#pragma once

#include <windows.h>

namespace bx::gdk {

// Dispatches public Microsoft PC GDK (Microsoft.GDK.Windows 2610.0.9661) COM IUnknown
// interfaces queried through QueryApiImpl. Returns true when (clsid, iid) matched and
// *out was populated, or sets *clsid_known=true when clsid matched but iid did not.
bool query_public_gdk_interface(const GUID* clsid, const GUID* iid, void** out, bool* clsid_known);

} // namespace bx::gdk
