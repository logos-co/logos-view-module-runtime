#include "QtCoreTokens.h"

#include "token_manager.h"

namespace logos::ui {

void saveCoreTokenToQtStore(const std::string& key, const std::string& token)
{
    TokenManager::instance().saveToken(key, token);
}

} // namespace logos::ui
