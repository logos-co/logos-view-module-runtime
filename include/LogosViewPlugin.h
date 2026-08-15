#pragma once

#include <QtPlugin>

class QObject;
class QRemoteObjectHostBase;

// The HOST side of this interface. The module side is declared separately, by
// logos-plugin-qt/cmake/LogosViewPluginBase.h.in; see the note in
// LogosViewReplicaFactory.h for why they cannot share a header, and for the
// `view-interface-abi` check in logos-module-builder that fails when the two
// declarations drift apart.
//
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
