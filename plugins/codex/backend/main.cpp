#include <QCommandLineParser>
#include <QCoreApplication>
#include <QThread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#include "codex/backend/adapter.hpp"

namespace {

constexpr qsizetype MAX_LINE_LENGTH = 16LL * 1024 * 1024;

void Send(const QByteArray& message) {
    if (message.isEmpty()) {
        return;
    }
    const auto size = static_cast<size_t>(message.size());
    if (std::fwrite(message.constData(), 1, size, stdout) != size ||
        std::fputc('\n', stdout) == EOF || std::fflush(stdout) == EOF) {
        // roostd has closed the output pipe. Do not wait on its input pipe.
        std::_Exit(EXIT_FAILURE);
    }
}

std::chrono::milliseconds Milliseconds(const QString& text,
                                       std::chrono::milliseconds fallback) {
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    return ok && value > 0 ? std::chrono::milliseconds(value) : fallback;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // Preserve UTF-8 and newline framing over redirected Windows handles.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    const QString program = QStringLiteral("codex.exe");
#else
    std::signal(SIGPIPE, SIG_IGN);
    const QString program = QStringLiteral("codex");
#endif
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Lists saved Codex sessions for roostd.");
    const QCommandLineOption pollMs("poll-ms", "Delay between lists.", "ms");
    const QCommandLineOption timeoutMs("timeout-ms",
                                       "Time allowed to list threads.", "ms");
    parser.addOptions({pollMs, timeoutMs});
    parser.addHelpOption();
    parser.process(app);

    roost::codex::Adapter adapter(
        qEnvironmentVariable("ROOST_CODEX_EXECUTABLE", program),
        Milliseconds(parser.value(pollMs), std::chrono::seconds(5)),
        Milliseconds(parser.value(timeoutMs), std::chrono::seconds(15)), Send);
    // Windows redirected stdin is a pipe handle, not a socket notifier.
    // Keep the blocking read off the event loop on all hosts.
    const std::unique_ptr<QThread> input(QThread::create([&app, &adapter] {
        QByteArray line;
        int code = EXIT_SUCCESS;
        for (int byte = std::fgetc(stdin); byte != EOF;
             byte = std::fgetc(stdin)) {
            if (byte == '\n') {
                if (!line.trimmed().isEmpty()) {
                    QMetaObject::invokeMethod(
                        &app,
                        [&adapter, message = std::move(line)] {
                            adapter.Handle(message).then(QtFuture::Launch::Sync,
                                                         Send);
                        },
                        Qt::QueuedConnection);
                }
                line.clear();
            } else if (line.size() == MAX_LINE_LENGTH) {
                std::fputs("codex-adapter: oversized request from roostd\n",
                           stderr);
                code = EXIT_FAILURE;
                break;
            } else {
                line.append(static_cast<char>(byte));
            }
        }
        QMetaObject::invokeMethod(
            &app, [code] { QCoreApplication::exit(code); },
            Qt::QueuedConnection);
    }));
    input->start();
    adapter.Start();
    const int result = QCoreApplication::exec();
    input->wait();
    return result;
}
