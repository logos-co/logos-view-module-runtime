#pragma once

#include <QtGlobal>

#if defined(LOGOS_VIEW_SURFACE_LIBRARY)
#  define LOGOS_VIEW_SURFACE_EXPORT Q_DECL_EXPORT
#else
#  define LOGOS_VIEW_SURFACE_EXPORT Q_DECL_IMPORT
#endif
