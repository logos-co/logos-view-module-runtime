#pragma once

#include <QString>

#include "UiPluginLoader.h"

namespace logos::ui {

// Builds a load request from a UI plugin on disk: a plugin library loaded as
// a legacy plugin, or a package directory holding metadata.json and/or
// manifest.json. DEV_QML_PATH, when it holds the view's entry file, replaces
// the installed view and turns on live reload (LOGOS_QML_HOT_RELOAD=0 opts
// out). Returns false with `error` set when the plugin cannot be loaded.
bool resolveUiPluginPath(const QString& path, UiPluginRequest* request, QString* error);

} // namespace logos::ui
