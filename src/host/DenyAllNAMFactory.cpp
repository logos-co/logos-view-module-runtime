#include "DenyAllNAMFactory.h"

#include "DenyAllNetworkAccessManager.h"

namespace logos::ui {

DenyAllNAMFactory::DenyAllNAMFactory(const QString& pluginLabel)
    : m_pluginLabel(pluginLabel)
{
}

QNetworkAccessManager* DenyAllNAMFactory::create(QObject* parent)
{
    return new DenyAllNetworkAccessManager(parent, m_pluginLabel);
}

} // namespace logos::ui
