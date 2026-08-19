// A minimal LogosViewReplicaFactory plugin for the replay test.
//
// The bridge only ever populates m_replicas through module(), which loads a
// factory plugin via QPluginLoader and asks it for a replica — so a test that
// needs a genuinely Valid replica in that map has to come in the same way a
// real view module does. This stands in for the repc-generated factory a view
// module ships: it hands back a dynamic replica, which reaches
// QRemoteObjectReplica::Valid against a published source exactly like a typed
// one does. Only the metaobject differs, and the replay loop does not look at
// it — it qobject_casts to QRemoteObjectReplica and reads state().

#include "LogosViewReplicaFactory.h"

#include <QObject>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QtPlugin>

class TestReplicaFactory : public QObject, public LogosViewReplicaFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LogosViewReplicaFactory_iid)
    Q_INTERFACES(LogosViewReplicaFactory)

public:
    QObject* acquire(QRemoteObjectNode* node) override
    {
        if (!node) return nullptr;
        return node->acquireDynamic(QStringLiteral("test_view"));
    }

    const QMetaObject* replicaMetaObject() const override
    {
        return &QRemoteObjectDynamicReplica::staticMetaObject;
    }
};

#include "test_replica_factory_plugin.moc"
