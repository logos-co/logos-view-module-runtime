#pragma once

// RFC-8 §4: what ui-host resolves from the LogosViewSurface library with
// QLibrary. Plain C so the symbol names are stable.

#include "SurfaceExport.h"

extern "C" {

// Installs the RFC-8 table on the QCoreApplication and, for transport
// "channel", connects to the host's QLocalServer `socketName` with `token`.
// For "wayland" it only records the mode (the window is shown as a toplevel
// on first use). Returns 0 on success, non-zero on failure. Must be called on
// the main thread after the application object exists.
LOGOS_VIEW_SURFACE_EXPORT int logos_view_surface_init(const char* token, const char* socketName,
                                                      const char* transport);

// Tears the session down (Bye) and removes the table.
LOGOS_VIEW_SURFACE_EXPORT void logos_view_surface_shutdown();

}

typedef int (*logos_view_surface_init_fn)(const char*, const char*, const char*);
typedef void (*logos_view_surface_shutdown_fn)();
