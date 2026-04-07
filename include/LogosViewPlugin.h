#pragma once

#include <QtPlugin>

class QObject;
class QRemoteObjectHostBase;

// Qt plugin interface that ui-host uses to wire a view-module plugin into a
// QRemoteObjectHost without reflection. A plugin implementing this interface:
//
//   1. Returns the QObject that QML should talk to via viewObject().
//   2. Performs *typed* QRemoteObject remoting against that object via
//      enableRemoting(host) — typically by calling the templated
//      host->enableRemoting<FooSourceAPI>(backend) overload.
class LogosViewPlugin {
public:
    virtual ~LogosViewPlugin() = default;

    virtual QObject* viewObject() = 0;
    virtual bool enableRemoting(QRemoteObjectHostBase* host) = 0;
};

#define LogosViewPlugin_iid "logos.view.plugin/1.0"
Q_DECLARE_INTERFACE(LogosViewPlugin, LogosViewPlugin_iid)
