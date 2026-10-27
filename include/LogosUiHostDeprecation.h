#pragma once

// ui-host is deprecated: an app's backend runs as a core module
// ("backend": "module" in its metadata.json), and ui-host, ViewModuleHost and
// LogosViewPlugin are removed in the next breaking release. Hosts that keep
// supporting ui-host apps until then define LOGOS_UI_HOST_ALLOWED.
#if defined(LOGOS_UI_HOST_ALLOWED)
#define LOGOS_UI_HOST_DEPRECATED
#else
#define LOGOS_UI_HOST_DEPRECATED                                                     \
    [[deprecated("ui-host is deprecated: run the app's backend as a core module "     \
                 "(\"backend\": \"module\"); removed in the next breaking release")]]
#endif
