#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>
#include <utility>

namespace {

QJsonObject Thread(const QString& id, const QString& preview,
                   const QString& status = "notLoaded") {
    return QJsonObject{{"id", id},
                       {"preview", preview},
                       {"createdAt", 1'730'831'111},
                       {"cwd", qEnvironmentVariable("ROOST_TEST_PROJECT")},
                       {"status", QJsonObject{{"type", status}}}};
}

void Write(const QJsonObject& message) {
    const QByteArray line =
        QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

// The test executable doubles as the Codex CLI. On the second poll the fixture
// changes a title, removes a thread and adds another one.
int AppServer() {
    int polls = 0;
    const QString scenario = qEnvironmentVariable("ROOST_TEST_SCENARIO");
    char input[4096];
    bool initialized = false;
    while (std::fgets(input, sizeof input, stdin) != nullptr) {
        const QJsonObject request = QJsonDocument::fromJson(input).object();
        const QString method = request.value("method").toString();
        const QJsonValue id = request.value("id");
        if (method == "initialize") {
            Write({{"id", id}, {"result", QJsonObject{}}});
            continue;
        }
        if (method == "initialized") {
            initialized = true;
            continue;
        }
        const QJsonObject params = request.value("params").toObject();
        if (!initialized || method != "thread/list" ||
            !params.value("modelProviders").isArray() ||
            !params.value("sourceKinds").toArray().contains("appServer")) {
            return 1;
        }
        if (scenario == "timeout") {
            continue;
        }
        if (params.contains("cursor")) {
            if (params.value("cursor") != "page+2") {
                return 1;
            }
            if (scenario == "bad-page" && polls > 1) {
                Write({{"id", id}, {"result", QJsonObject{{"data", false}}}});
                continue;
            }
            Write(
                {{"id", id},
                 {"result",
                  QJsonObject{{"nextCursor", QJsonValue::Null},
                              {"data", QJsonArray{Thread("old", "Older")}}}}});
            continue;
        }
        ++polls;
        QJsonObject first = Thread("thr_first", "First prompt");
        if (polls > 1) {
            first.insert("name", "Renamed");
            first.insert(
                "status",
                QJsonObject{{"type", "active"},
                            {"activeFlags", QJsonArray{"waitingOnApproval"}}});
        }
        QJsonArray data{first};
        if (polls > 1 && scenario != "bad-page") {
            data.append(Thread("new", "New prompt", "systemError"));
        }
        const QJsonValue cursor = polls == 1 || scenario == "bad-page"
                                      ? QJsonValue("page+2")
                                      : QJsonValue(QJsonValue::Null);
        Write({{"method", "thread/status/changed"}, {"params", QJsonObject{}}});
        Write(
            {{"id", id},
             {"result", QJsonObject{{"data", data}, {"nextCursor", cursor}}}});
    }
    return 0;
}

class CodexAdapterTest : public QObject {
    Q_OBJECT

public:
    explicit CodexAdapterTest(QString adapterPath)
        : AdapterPath_(std::move(adapterPath)) {}

private:
    QString AdapterPath_;

    bool Launch(const QString& scenario, const QString& program,
                const QString& pollMs = "500",
                const QString& timeoutMs = "4000") {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_TEST_PROJECT",
                           QDir::toNativeSeparators(Project_.path()));
        environment.insert("ROOST_TEST_SCENARIO", scenario);
        Process_.setProcessEnvironment(environment);
        Process_.start(AdapterPath_, {"--program", program, "--poll-ms", pollMs,
                                      "--timeout-ms", timeoutMs});
        return Process_.waitForStarted();
    }

    QJsonObject Read() {
        QElapsedTimer timer;
        timer.start();
        while (!Process_.canReadLine()) {
            const auto remaining = 5000 - timer.elapsed();
            if (remaining <= 0 ||
                !Process_.waitForReadyRead(static_cast<int>(remaining))) {
                return {};
            }
        }
        return QJsonDocument::fromJson(Process_.readLine()).object();
    }

    QJsonObject Call(const QString& method, QJsonObject params = {}) {
        params.insert("agent", "codex");
        const int id = ++NextId_;
        const QByteArray request =
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"method", method},
                                      {"params", params}})
                .toJson(QJsonDocument::Compact) +
            '\n';
        Process_.write(request);
        for (QJsonObject answer = Read(); !answer.isEmpty(); answer = Read()) {
            if (answer.value("id") == id) {
                return answer;
            }
            Events_.append(answer);
        }
        return {};
    }

    QTemporaryDir Project_{QDir::temp().filePath("roost codex project-XXXXXX")};
    QProcess Process_;
    QList<QJsonObject> Events_;
    int NextId_ = 0;

private slots:
    void init() {
        QVERIFY(Project_.isValid());
        Events_.clear();
    }

    void cleanup() {
        Process_.closeWriteChannel();
        QVERIFY2(Process_.waitForFinished(5000),
                 Process_.readAllStandardError().constData());
        QCOMPARE(Process_.exitCode(), 0);
        Process_.readAll();
    }

    void ListsPagesAndResumesById() {
        const QString cli = QCoreApplication::applicationFilePath();
        QVERIFY(Launch({}, cli));
        const QJsonObject listed = Call("agent.listSessions");
        const QJsonArray rows = listed.value("result").toArray();
        QCOMPARE(rows.size(), 2);
        const QJsonObject first = rows.first().toObject();
        QCOMPARE(first.value("id"), QJsonValue("thr_first"));
        QCOMPARE(first.value("title"), QJsonValue("First prompt"));
        QCOMPARE(first.value("cwd"),
                 QJsonValue(QDir::toNativeSeparators(Project_.path())));
        QCOMPARE(first.value("state"), QJsonValue("idle"));
        QCOMPARE(first.value("startedAt").toInteger(), 1'730'831'111'000LL);
        QCOMPARE(rows.last().toObject().value("id"), QJsonValue("old"));

        const auto attached =
            Call("agent.attachCommand", {{"session", "thr_first"}})
                .value("result")
                .toObject();
        QCOMPARE(QDir::cleanPath(attached.value("program").toString()),
                 QDir::cleanPath(cli));
        QCOMPARE(attached.value("args").toArray(),
                 QJsonArray({"resume", "thr_first"}));
        QCOMPARE(attached.value("cwd"),
                 QJsonValue(QDir::toNativeSeparators(Project_.path())));
        QVERIFY(Call("agent.attachCommand", {{"session", "missing"}})
                    .value("result")
                    .isNull());
        QCOMPARE(Call("agent.attachCommand", {{"session", "--last"}})
                     .value("error")
                     .toObject()
                     .value("code")
                     .toInt(),
                 -32602);

        while (Events_.size() < 3) {
            const QJsonObject event = Read();
            QVERIFY(!event.isEmpty());
            Events_.append(event);
        }
        QCOMPARE(Events_[0].value("method"),
                 QJsonValue("session.stateChanged"));
        const auto changed =
            Events_[0].value("params").toObject().value("session").toObject();
        QCOMPARE(changed.value("title"), QJsonValue("Renamed"));
        QCOMPARE(changed.value("state"), QJsonValue("blocked"));
        const auto added =
            Events_[1].value("params").toObject().value("session").toObject();
        QCOMPARE(added.value("id"), QJsonValue("new"));
        QCOMPARE(added.value("state"), QJsonValue("failed"));
        QCOMPARE(Events_[2].value("method"), QJsonValue("session.removed"));
        QCOMPARE(Events_[2].value("params").toObject().value("session"),
                 QJsonValue("old"));
    }

    void MissingCliIsOptional() {
        QVERIFY(Launch({}, Project_.filePath("missing-codex")));
        QCOMPARE(Call("agent.listSessions").value("result"),
                 QJsonValue(QJsonArray{}));
    }

    void AFailedSecondPageDoesNotPublishAPartialSnapshot() {
        QVERIFY(Launch("bad-page", QCoreApplication::applicationFilePath(),
                       "1000", "4000"));
        QCOMPARE(Call("agent.listSessions").value("result").toArray().size(),
                 2);
        QJsonObject response;
        QTRY_VERIFY_WITH_TIMEOUT(
            (response = Call("agent.listSessions")).contains("error"), 4000);
        QVERIFY(response.value("error")
                    .toObject()
                    .value("message")
                    .toString()
                    .contains("invalid thread/list page"));
        QVERIFY(Events_.isEmpty());
    }

    void ATimeoutReleasesWaitingRequests() {
        QVERIFY(Launch("timeout", QCoreApplication::applicationFilePath(),
                       "500", "350"));
        const QJsonObject error =
            Call("agent.listSessions").value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32603);
        QVERIFY(error.value("message").toString().contains("timed out"));
    }
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 2) {
        return 1;
    }
    if (arguments[1] == "app-server") {
        return AppServer();
    }
    CodexAdapterTest test(arguments[1]);
    return QTest::qExec(&test, {arguments.front()});
}

#include "adapter_test.moc"
