#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <chrono>

#include "core/version.hpp"

namespace {

// Long enough for a cold start of the software renderer on a loaded CI runner.
constexpr std::chrono::seconds SMOKE_TIMEOUT{10};

// --smoke: exit with 0 once the window has shown its first frame, with 1 if it
// has not within SMOKE_TIMEOUT. Proves that Qt, the QML module and the scene
// graph load and render, which is all M0 has to prove.
void ExitAfterFirstFrame(QQuickWindow* window) {
    // frameSwapped comes from the render thread; queued, so exit runs on ours.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, qApp,
        [] { QCoreApplication::exit(0); }, Qt::QueuedConnection);
    QTimer::singleShot(SMOKE_TIMEOUT, qApp, [] {
        qCritical("roost --smoke: no frame within %lld s",
                  static_cast<long long>(SMOKE_TIMEOUT.count()));
        QCoreApplication::exit(1);
    });
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("roost"));
    QCoreApplication::setApplicationVersion(
        QString::fromStdString(roost::VersionString()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Background coding-agent sessions on the desktop"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption smoke(
        QStringLiteral("smoke"),
        QStringLiteral("Exit after the first frame is shown (CI check)."));
    parser.addOption(smoke);
    parser.process(app);

    QQmlApplicationEngine engine;
    engine.loadFromModule("Roost.Shell", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    if (parser.isSet(smoke)) {
        auto* window =
            qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (window == nullptr) {
            qCritical("roost --smoke: the root object is not a window");
            return 1;
        }
        ExitAfterFirstFrame(window);
    }

    return QGuiApplication::exec();
}
