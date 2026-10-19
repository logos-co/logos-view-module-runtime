#pragma once

#include <string>

namespace logos::ui {

// A logos::host::LogosCore::Config::tokenListener for Qt hosts: saves each
// token core issues into this process's TokenManager, the store the host's
// LogosAPI and logos::admitConsumer read.
void saveCoreTokenToQtStore(const std::string& key, const std::string& token);

} // namespace logos::ui
