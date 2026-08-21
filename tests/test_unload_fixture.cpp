// A view plugin whose ONLY job is to record what ui-host did to it on the way
// down, and in what order.
//
// Built three times, differing only in UNLOAD_MODE:
//
//   0  none      no teardown hook at all -- the module that predates it, and
//                the case that has to stay free. ui-host's invokeMethod finds
//                no such meta-method and moves on.
//   1  finishes  Asynchronous, completes after ~1s.
//   2  hangs     Asynchronous, never signals. The host must give up on it.
//
// WHY A JOURNAL, and why a HEARTBEAT.
//
// A host-side call that finds no meta-method is a silent no-op, and it looks
// exactly like success from the outside: the process still exits 0, still on
// time. Elapsed wall-clock cannot tell the two apart either -- a host that
// never waited and a module that finished instantly produce the same number.
//
// So each plugin appends to a journal file, and every Asynchronous mode also
// arms a repeating heartbeat in aboutToUnload(). Ticks can only be written by
// an event loop that is actually running, which is the thing under test: the
// nested QEventLoop the host spins while it waits. Their ORDER against
// "work-done" and "dtor", and the RELATIVE count between the two async modes,
// establish the behaviour without a single timing assertion:
//
//   * "work-done" before "dtor"          -> the host waited for the completion
//   * ticks present in the hang case     -> the host really did wait, not skip
//   * hang ticks > finish ticks          -> the deadline outlasts a completion,
//                                           i.e. giving up is a bound and not
//                                           the mechanism
#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QtPlugin>

#include <cstdio>
#include <cstdlib>

#ifndef UNLOAD_MODE
#error "UNLOAD_MODE must be defined (0=none, 1=finishes, 2=hangs)"
#endif

namespace {

// Append one line and flush. Line-at-a-time and unbuffered on purpose: the
// hang case exists to be abandoned mid-teardown, and a journal still sitting
// in stdio's buffer when that happens would lose exactly the evidence the test
// is here to read.
void journal(const char* entry)
{
    const char* path = ::getenv("LOGOS_TEST_UNLOAD_JOURNAL");
    if (!path || !*path) return;
    FILE* f = ::fopen(path, "a");
    if (!f) return;
    ::fprintf(f, "%s\n", entry);
    ::fflush(f);
    ::fclose(f);
}

// How often the heartbeat writes while the host is waiting. Small enough that
// the ~1s completion and the 2s deadline land far apart in tick counts, which
// is what lets the test compare the two runs instead of trusting a clock.
constexpr int kHeartbeatMs = 100;

// When the "finishes" mode reports completion. Comfortably inside ui-host's
// 2s grace period, and comfortably outside the noise of a single heartbeat.
constexpr int kWorkMs = 1000;

} // namespace

class UnloadFixturePlugin : public QObject
{
    Q_OBJECT
    // No FILE: ui-host never reads a view plugin's metadata, it loads the
    // instance and remotes it. One IID across all three modes is fine -- they
    // are three separate plugin files loaded by three separate processes, and
    // Qt keys its plugin cache on the file path.
    //
    // No Q_INTERFACES either, so ui-host's qobject_cast<LogosViewPlugin*> fails
    // and it falls back to dynamic remoting. That is the path a view plugin
    // without a .rep takes, and it keeps this fixture free of the generated
    // ViewPluginBase machinery that has nothing to do with teardown.
    Q_PLUGIN_METADATA(IID "org.logos.test.UnloadFixture")

public:
    UnloadFixturePlugin() = default;

    ~UnloadFixturePlugin() override
    {
        // The last thing that happens. Everything the host did for this plugin
        // must appear above this line.
        journal("dtor");
    }

    // Deliberately absent in mode 0, so that build produces a plugin whose
    // meta-object genuinely has no such method -- the real "module predates
    // the hook" shape, not a stub that returns Synchronous.
#if UNLOAD_MODE != 0
    Q_INVOKABLE int aboutToUnload()
    {
        journal("about-to-unload");

        // Armed here rather than in the constructor: before this point the
        // application event loop is running normally and ticks would say
        // nothing. From here on, the only loop that can run it is the nested
        // one the host spins to wait for us.
        auto* beat = new QTimer(this);
        connect(beat, &QTimer::timeout, this, []() { journal("tick"); });
        beat->start(kHeartbeatMs);

#if UNLOAD_MODE == 1
        // Finish, late but inside the grace period.
        QTimer::singleShot(kWorkMs, this, [this]() {
            journal("work-done");
            emit unloadFinished();
            journal("signalled");
        });
#endif
        // 1 == LogosShutdown::Asynchronous. Sent as a bare int because that is
        // what the host reads back with Q_RETURN_ARG(int) -- it resolves this
        // by signature and must not need an SDK enum to do it.
        return 1;
    }

Q_SIGNALS:
    void unloadFinished();
#endif
};

#include "test_unload_fixture.moc"
