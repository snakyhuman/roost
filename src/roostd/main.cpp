#include <QCommandLineParser>
#include <QCoreApplication>

#include "core/version.hpp"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("roostd"));
    QCoreApplication::setApplicationVersion(
        QString::fromStdString(roost::VersionString()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("roost daemon: agent sessions, PTYs, plugin backends"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    return QCoreApplication::exec();
}
