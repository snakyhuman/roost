// A one-off capture of every screen and visual state of the roost window, for
// a designer who redraws the UI. Not a test: it asserts nothing about the
// product and is not registered with ctest. Every case boots the real window
// (Main.qml, the real panels) against a real roostd with a fake `claude`, on
// the offscreen platform, drawn through Qt Quick's RHI renderer (Metal on
// macOS, the path users see; the software scene graph draws TerminalItem
// wrongly) by a QQuickRenderControl into a texture, and saves it as
// <out>/NN-<state>@<dpr>x.png. Session names and terminal text are made up.
//
//   tools/ui_inventory.sh <out-dir>        (runs it at scale 1 and 2)
//
// Environment: UI_INVENTORY_OUT is the directory of the PNGs;
// QT_SCALE_FACTOR=2 gives the @2x images; QT_QUICK_BACKEND=rhi selects RHI.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <rhi/qrhi.h>
#include <signal.h>

#include "core/vt_render_state.hpp"
#include "core/vt_terminal.hpp"
#include "helpers/refusing_server.hpp"
#include "helpers/sessions_window.hpp"
#include "helpers/window_teardown.hpp"
#include "roost/single_session.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// Draws the items of a window that is never shown through QRhi (Metal).
// On the offscreen platform a shown window has no Metal layer, and its render
// thread crashes in the swap chain; the software scene graph draws
// TerminalItem wrongly. So the root window of Main.qml stays hidden, its
// content items move to a window of this class that has a QQuickRenderControl,
// and a timer polishes it (layouts are laid out on polish) while the test
// waits. Input goes to this window (SessionsWindow::Window).
class Mirror {
public:
    explicit Mirror(QQuickWindow* root)
        : Win_(&Control_), Content_(root->contentItem()) {
        Win_.setGeometry(0, 0, root->width(), root->height());
        Win_.setColor(root->color());
        Ready_ = Control_.initialize();
        Items_ = Content_->childItems();
        // `anchors.fill: parent` follows the parent by itself.
        for (QQuickItem* item : Items_) {
            item->setParentItem(Win_.contentItem());
        }
        Fit();
        QFocusEvent focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(&Win_, &focus);
        QObject::connect(&Tick_, &QTimer::timeout, &Win_, [this] {
            Fit();
            Control_.polishItems();
        });
        Tick_.start(16);
    }

    // The items go back to the root, which owns them.
    ~Mirror() {
        Tick_.stop();
        for (QQuickItem* item : Items_) {
            item->setParentItem(Content_);
        }
    }

    bool Ready() const { return Ready_; }
    QQuickWindow* Window() { return &Win_; }

    // A window that is not shown gets no resize event, which is what sizes
    // its content item.
    void Fit() { Win_.contentItem()->setSize(Win_.size()); }

    // One frame into a texture of the window's size, read back.
    QImage Grab() {
        QRhi* rhi = Control_.rhi();
        if (rhi == nullptr) {
            return {};
        }
        Fit();
        const qreal dpr = Win_.devicePixelRatio();
        const QSize size(qCeil(Win_.width() * dpr), qCeil(Win_.height() * dpr));
        std::unique_ptr<QRhiTexture> texture(rhi->newTexture(
            QRhiTexture::RGBA8, size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!texture->create()) {
            return {};
        }
        std::unique_ptr<QRhiRenderBuffer> stencil(
            rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!stencil->create()) {
            return {};
        }
        QRhiTextureRenderTargetDescription description{
            QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(stencil.get());
        std::unique_ptr<QRhiTextureRenderTarget> target(
            rhi->newTextureRenderTarget(description));
        std::unique_ptr<QRhiRenderPassDescriptor> pass(
            target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        if (!target->create()) {
            return {};
        }
        QQuickRenderTarget renderTarget =
            QQuickRenderTarget::fromRhiRenderTarget(target.get());
        renderTarget.setDevicePixelRatio(dpr);
        Win_.setRenderTarget(renderTarget);

        Control_.polishItems();
        // The control owns its QRhi and starts and ends the offscreen frame.
        Control_.beginFrame();
        Control_.sync();
        Control_.render();
        QRhiReadbackResult read;
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        updates->readBackTexture(QRhiReadbackDescription(texture.get()), &read);
        Control_.commandBuffer()->resourceUpdate(updates);
        Control_.endFrame();
        Win_.setRenderTarget(QQuickRenderTarget());
        if (read.data.isEmpty()) {
            return {};
        }
        QImage image(reinterpret_cast<const uchar*>(read.data.constData()),
                     read.pixelSize.width(), read.pixelSize.height(),
                     QImage::Format_RGBA8888_Premultiplied);
        image = image.copy();
        if (rhi->isYUpInFramebuffer()) {
            image = image.flipped(Qt::Vertical);
        }
        return image;
    }

private:
    QQuickRenderControl Control_;
    QQuickWindow Win_;
    QQuickItem* Content_;
    QList<QQuickItem*> Items_;
    QTimer Tick_;
    bool Ready_ = false;
};

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 800;

// The sessions of the made-up project: id, name, directory, state.
struct Fake {
    const char* Id;
    const char* Name;
    const char* Cwd;
    const char* State;
};
constexpr Fake SESSIONS[] = {
    {"bg-a1", "refactor-parser", "/work/parser", "working"},
    {"bg-a2", "fix-login-flow", "/work/auth", "blocked"},
    {"bg-a3", "add-retry-logic", "/work/http-client", "failed"},
    {"bg-a4", "update-docs", "/work/docs", "done"},
    {"bg-a5", "migrate-schema", "/work/db", "idle"},
    {"bg-a6", "write-tests", "/work/parser", "working"},
    {"bg-a7", "speed-up-ci", "/work/ci", "done"},
    {"bg-a8", "", "/work/scratch", "blocked"},
};
constexpr int SESSION_COUNT = sizeof(SESSIONS) / sizeof(SESSIONS[0]);

QString KeyOf(const char* id) { return QStringLiteral("claude/") + id; }

// A screen of a TUI that draws like an agent's: palette rows, attributes, a
// box, a diff, 256 colours, a truecolour ramp, and a prompt the cursor rests
// on. The attach of a done session (a7) prints a line and exits.
constexpr const char* SAMPLE = R"sh(#!/bin/sh
printf '\033[2J\033[H'
printf ' \033[1;38;5;75mClaude Code\033[0m  \033[2mrefactor-parser\033[0m   \033[1;32m* working\033[0m  \033[33m12k tokens\033[0m\n\n'
printf ' normal  '; for i in 0 1 2 3 4 5 6 7; do printf '\033[3%dm## %d \033[0m' $i $i; done; printf '\n'
printf ' bright  '; for i in 0 1 2 3 4 5 6 7; do printf '\033[9%dm## %d \033[0m' $i $i; done; printf '\n'
printf ' bg      '; for i in 0 1 2 3 4 5 6 7; do printf '\033[4%dm %d \033[0m ' $i $i; done; printf '\n'
printf ' bright  '; for i in 0 1 2 3 4 5 6 7; do printf '\033[10%dm %d \033[0m ' $i $i; done; printf '\n\n'
printf ' \033[1mbold\033[0m  \033[2mdim\033[0m  \033[3mitalic\033[0m  \033[4munderline\033[0m  \033[7minverse\033[0m  \033[9mstrike\033[0m  \033[1;4;31mbold underline red\033[0m  \033[1;3;36mbold italic cyan\033[0m\n\n'
printf ' \033[90m+------------------------------------------------------------+\033[0m\n'
printf ' \033[90m|\033[0m \033[1;33m*\033[0m Edit \033[36msrc/parser/lexer.cpp\033[0m                       \033[32m+12\033[0m \033[31m-3\033[0m \033[90m|\033[0m\n'
printf ' \033[90m|\033[0m \033[31m- auto token = next(input);\033[0m                                 \033[90m|\033[0m\n'
printf ' \033[90m|\033[0m \033[32m+ auto token = lexer.Next(input);\033[0m                           \033[90m|\033[0m\n'
printf ' \033[90m|\033[0m \033[41;97m FAIL \033[0m  test_lexer.cpp:88  expected 3, got 4              \033[90m|\033[0m\n'
printf ' \033[90m|\033[0m \033[42;30m  OK  \033[0m  test_parser.cpp  17 passed                        \033[90m|\033[0m\n'
printf ' \033[90m+------------------------------------------------------------+\033[0m\n\n'
printf ' 256   '; i=16; while [ $i -lt 52 ]; do printf '\033[48;5;%dm \033[0m' $i; i=$((i+1)); done
printf '  '; i=232; while [ $i -lt 256 ]; do printf '\033[48;5;%dm \033[0m' $i; i=$((i+1)); done; printf '\n'
printf ' true  '; i=0; while [ $i -lt 60 ]; do printf '\033[48;2;%d;%d;%dm \033[0m' $((i*4)) $((255-i*4)) 160; i=$((i+1)); done; printf '\n\n'
printf ' \033[1;36m>\033[0m run the tests and fix what fails'
exec sleep 3600
)sh";

constexpr const char* ENDED = R"sh(#!/bin/sh
printf '\033[2J\033[H\n \033[1mspeed-up-ci\033[0m\n\n \033[32m* all jobs finished\033[0m\n \033[2mSession closed.\033[0m\n'
exit 0
)sh";

constexpr const char* CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach)
    cd /
    case "$2" in
    bg-a7) exec "$dir/ended.sh" ;;
    *) exec "$dir/sample.sh" ;;
    esac ;;
rm|stop|respawn) exit 0 ;;
*) exit 0 ;;
esac
)sh";

bool WriteExecutable(const QString& path, const char* text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) >= 0 &&
           file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                               QFile::ExeOwner);
}

}  // namespace

class UiInventory : public SessionsWindow {
    Q_OBJECT

public:
    QDir Out_;
    int Number_ = 0;
    QStringList Taken_;
    QStringList Missed_;

    std::unique_ptr<Mirror> Mirror_;

    QQuickWindow* Window() override {
        return Mirror_ ? Mirror_->Window() : SessionsWindow::Window();
    }

    void Adopt(QQuickWindow* root) override {
        Mirror_ = std::make_unique<Mirror>(root);
        QVERIFY(Mirror_->Ready());
    }

    // Ctrl+\ is a window shortcut and needs the window to be the active one;
    // the shortcut's handler is the same call.
    void ToList() {
        QVERIFY(QMetaObject::invokeMethod(Slot(LIST), "forceActiveFocus"));
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
    }

    // Saves what the window shows now; the number follows the order of the
    // cases, so a re-run gives the same names.
    void Shot(const QString& name) {
        QTest::qWait(450);
        QQuickWindow* window = Window();
        QVERIFY(window != nullptr && Mirror_ != nullptr);
        QImage image = Mirror_->Grab();
        QVERIFY(!image.isNull());
        const int scale = qRound(window->devicePixelRatio());
        const QString file = QStringLiteral("%1-%2@%3x.png")
                                 .arg(++Number_, 2, 10, QLatin1Char('0'))
                                 .arg(name)
                                 .arg(scale);
        QVERIFY(image.save(Out_.filePath(file)));
        Taken_ << QStringLiteral("%1 %2x%3")
                      .arg(file)
                      .arg(image.width())
                      .arg(image.height());
    }

    void Skip(const QString& name, const QString& why) {
        Missed_ << name + ": " + why;
    }

    // The attach and the list the fake claude serves.
    bool WriteScene(const char* claude = CLAUDE) {
        if (!WriteFakeClaude(claude)) {
            return false;
        }
        QJsonArray agents;
        qint64 at = 1790150000000;
        for (const Fake& fake : SESSIONS) {
            agents.append(
                QJsonObject{{"id", fake.Id},
                            {"cwd", fake.Cwd},
                            {"kind", "background"},
                            {"startedAt", at += 100000},
                            {"sessionId", QString("0000-%1").arg(fake.Id)},
                            {"name", fake.Name},
                            {"state", fake.State}});
        }
        QSaveFile list(AgentsPath());
        const QDir bin(Dir_->filePath("bin"));
        QSaveFile sample(bin.filePath("sample.sh"));
        return list.open(QIODevice::WriteOnly) &&
               list.write(QJsonDocument(agents).toJson()) >= 0 &&
               list.commit() &&
               WriteExecutable(bin.filePath("sample.sh"), SAMPLE) &&
               WriteExecutable(bin.filePath("ended.sh"), ENDED);
    }

    // The document `sessions` as the daemon finds it at the start.
    bool Seed(const QJsonObject& sections) {
        QJsonObject document = sections;
        document.insert("version", 1);
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/sessions.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(document).toJson()) >= 0 &&
               file.commit();
    }

    static QJsonObject Doc() {
        return {{"categories", QJsonObject{{KeyOf("bg-a1"), "backend"},
                                           {KeyOf("bg-a5"), "backend"},
                                           {KeyOf("bg-a6"), "backend"},
                                           {KeyOf("bg-a2"), "auth"},
                                           {KeyOf("bg-a4"), "docs"}}},
                {"unread", QJsonArray{KeyOf("bg-a4"), KeyOf("bg-a7")}},
                {"states", QJsonObject{{KeyOf("bg-a4"), "working"},
                                       {KeyOf("bg-a7"), "working"}}}};
    }

    void BootScene(const QJsonObject& document, int rows = SESSION_COUNT,
                   const char* claude = CLAUDE) {
        QVERIFY(WriteScene(claude));
        QVERIFY(Seed(document));
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Count(), rows, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                     ->findChild<QObject*>("sessionsModel")
                                     ->property("ready")
                                     .toBool(),
                                 TIMEOUT_MS);
        Window()->resize(WIDTH, HEIGHT);
        QTest::qWait(300);
    }

    // The window without a daemon: no panels, the banner and the placeholders.
    void OpenBare() {
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        Singleton_->setApi(*Api_);
        Api_->Connect(SocketPath());
        Engine_->setInitialProperties({{"visible", false}});
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ = std::make_unique<Shell>(*Engine_->rootObjects().first(),
                                         *Loader_, *Api_, SocketPath());
        auto* window = qobject_cast<QQuickWindow*>(Root());
        QVERIFY(window != nullptr);
        Adopt(window);
        Window()->resize(WIDTH, HEIGHT);
        QTest::qWait(300);
    }

    QString RootText() const {
        return const_cast<UiInventory*>(this)
            ->Root()
            ->property("connection")
            .toString();
    }

    void Type(const QString& text) {
        for (const QChar c : text) {
            QTest::keyClick(Window(), c.toLatin1());
        }
    }

    void Question() {
        PressNative(Qt::Key_Question, "?", 0x2C, 61, Qt::ShiftModifier);
    }

    void MoveToId(const char* id) {
        for (int step = 0; step < 16 && CursorId() != id; ++step) {
            Press(Qt::Key_Down);
        }
        for (int step = 0; step < 16 && CursorId() != id; ++step) {
            Press(Qt::Key_Up);
        }
        QCOMPARE(CursorId(), QString(id));
    }

    // Opens the session under the cursor and waits for its first screen.
    void OpenCurrent(const QString& marker) {
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        if (!marker.isEmpty()) {
            QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(marker), TIMEOUT_MS);
        }
        QTest::qWait(300);
    }

    QPoint CellPoint(int column, int row) {
        TerminalItem* terminal = Terminal();
        return terminal
            ->mapToScene(QPointF((column + 0.5) * terminal->cellWidth(),
                                 (row + 0.5) * terminal->cellHeight()))
            .toPoint();
    }

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
        const QString out = qEnvironmentVariable(
            "UI_INVENTORY_OUT", QDir::current().filePath("ui-inventory-out"));
        Out_ = QDir(out);
        QVERIFY(Out_.mkpath("."));
    }

    void init() {
        Prepare();
        Unexposed_ = true;
    }
    void cleanup() {
        Mirror_.reset();
        Teardown();
    }

    void cleanupTestCase() {
        QFile index(Out_.filePath("capture-log.txt"));
        if (index.open(QIODevice::Append | QIODevice::Text)) {
            index.write(("# taken\n" + Taken_.join('\n') +
                         "\n# not captured\n" + Missed_.join('\n') + "\n")
                            .toUtf8());
        }
    }

    // ---- the window with no roostd ----

    void ConnectingNoDaemon() {
        OpenBare();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shot("window-connecting-no-roostd");
        Root()->setProperty("connection", "starting");
        Shot("banner-starting-roostd");
        Root()->setProperty("connection", "restarting");
        Shot("banner-restarting-roostd");
        Root()->setProperty("connection", "startFailed");
        Root()->setProperty(
            "connectionMessage",
            "roostd did not start: cannot execute /usr/local/bin/roostd");
        Shot("banner-start-failed");
        Root()->setProperty("connection", "disconnected");
        Root()->setProperty("connectionMessage", "connection closed by roostd");
        Shot("banner-disconnected-no-panels");
    }

    void RefusedVersionMismatch() {
        RefusingServer server;
        QVERIFY(server.Listen(SocketPath()));
        OpenBare();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(RootText(), QString("refused"), TIMEOUT_MS);
        Shot("banner-refused-retry-only");
        // With a launcher the shell adds the restart text and the button; the
        // properties are written as Shell::SetConnection writes them.
        QString message = Root()->property("connectionMessage").toString();
        message.replace(
            ". Restart roostd, then press Retry.",
            "\nRestarting closes the warm terminals; Claude sessions "
            "keep running.");
        Root()->setProperty("connectionMessage", message);
        Root()->setProperty("restartOffered", true);
        Shot("banner-refused-restart-roostd");
    }

    // ---- the window with sessions ----

    void ListAndPrompts() {
        BootScene(Doc());
        if (QTest::currentTestFailed()) {
            return;
        }
        Shot("list-initial-nothing-open");
        Press(Qt::Key_Down);
        Press(Qt::Key_Down);
        Shot("list-cursor-on-session");
        Press(Qt::Key_M);
        Shot("list-compact");
        Press(Qt::Key_M);
        Press(Qt::Key_A);
        Shot("tab-active");
        Press(Qt::Key_A);
        Shot("tab-unread");
        Press(Qt::Key_A);

        Press(Qt::Key_Slash);
        Shot("prompt-search-empty");
        Type("fix");
        Shot("prompt-search-typed-live-filter");
        Press(Qt::Key_Return);
        Shot("search-kept-after-enter");
        Press(Qt::Key_Escape);
        Press(Qt::Key_Slash);
        Type("zzz");
        Shot("search-no-results");
        Press(Qt::Key_Escape);
        QTest::qWait(200);

        MoveToId("bg-a1");
        Press(Qt::Key_C);
        Shot("prompt-category-with-known-hint");
        Press(Qt::Key_Escape);
        Press(Qt::Key_E);
        Shot("prompt-rename");
        Press(Qt::Key_Escape);
        Press(Qt::Key_N);
        Shot("prompt-new-session-task");
        Type("add tests for the lexer");
        Press(Qt::Key_Return);
        Shot("prompt-new-session-category");
        Press(Qt::Key_Escape);

        Press(Qt::Key_D);
        Shot("confirm-delete");
        Press(Qt::Key_Escape);
        Press(Qt::Key_X);
        Shot("confirm-archive");
        Press(Qt::Key_Escape);
        Press(Qt::Key_R);
        Shot("confirm-respawn");
        Press(Qt::Key_Escape);
        Press(Qt::Key_R, Qt::ShiftModifier);
        Shot("confirm-respawn-all");
        Press(Qt::Key_Escape);

        Question();
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)->property("helpOpen").toBool(),
                                 TIMEOUT_MS);
        Shot("help-overlay");
        Press(Qt::Key_Escape);

        for (int i = 0; i < 3; ++i) {
            Press(Qt::Key_Period);
        }
        Shot("list-widened");
        for (int i = 0; i < 6; ++i) {
            Press(Qt::Key_Comma);
        }
        Shot("list-narrowed");
        for (int i = 0; i < 3; ++i) {
            Press(Qt::Key_Period);
        }

        // Fold, pin, unread count on a folded header.
        MoveToId("bg-a4");
        Press(Qt::Key_P);
        Shot("category-pinned");
        Press(Qt::Key_Z);
        Shot("category-folded-with-unread-and-pin");
        Press(Qt::Key_Z, Qt::ShiftModifier);
        QTest::qWait(200);

        // A command and its notice: r then y.
        MoveToId("bg-a2");
        Press(Qt::Key_R);
        Press(Qt::Key_Y);
        QTRY_VERIFY_WITH_TIMEOUT(
            !PanelOf(LIST)->property("notice").toString().isEmpty(),
            TIMEOUT_MS);
        Shot("notice-after-command");
    }

    void TerminalStates() {
        BootScene(Doc());
        if (QTest::currentTestFailed()) {
            return;
        }
        MoveToId("bg-a1");
        Press(Qt::Key_Return);
        Skip("terminal-opening",
             "the status Opening <agent>/<id> lasts less than one event-loop "
             "turn on a local roostd; no rendered frame shows it");
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains("run the tests"),
                                 TIMEOUT_MS);
        QTest::qWait(400);
        Shot("terminal-open-ansi-sample-focus-terminal");

        // A selection by drag: rows 3 to 6 of the sample.
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier,
                          CellPoint(10, 3));
        QTest::mouseMove(Window(), CellPoint(40, 4));
        QTest::mouseMove(Window(), CellPoint(60, 6));
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            CellPoint(60, 6));
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasSelection(), TIMEOUT_MS);
        Shot("terminal-selection");

        ToList();
        Shot("list-open-session-focus-list");

        // An ended session.
        MoveToId("bg-a7");
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("ended"), TIMEOUT_MS);
        Shot("terminal-session-ended");

        // A session roostd does not know: the open fails.
        PanelOf(TERMINAL)->setProperty(
            "session", QVariantMap{{"agent", "claude"}, {"id", "bg-gone"}});
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("failed"), TIMEOUT_MS);
        Shot("terminal-open-failed");
    }

    void OpenSessionHighlightInListAndDelete() {
        BootScene(Doc());
        if (QTest::currentTestFailed()) {
            return;
        }
        MoveToId("bg-a2");
        OpenCurrent("run the tests");
        ToList();
        Press(Qt::Key_Down);
        Shot("list-open-marker-and-cursor-elsewhere");
        Press(Qt::Key_D);
        Press(Qt::Key_Y);
        QTRY_VERIFY_WITH_TIMEOUT(
            !PanelOf(LIST)->property("notice").toString().isEmpty(),
            TIMEOUT_MS);
        Shot("notice-delete");
    }

    void ArchiveView() {
        QJsonObject doc = Doc();
        doc.insert("archived", QJsonArray{KeyOf("bg-a3"), KeyOf("bg-a7")});
        BootScene(doc, SESSION_COUNT - 2);
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_V);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 2, TIMEOUT_MS);
        Shot("archive-view");
    }

    void EmptyList() {
        QVERIFY(WriteScene());
        QVERIFY(ListWithout(ORDER));
        QJsonArray none;
        QSaveFile list(AgentsPath());
        QVERIFY(list.open(QIODevice::WriteOnly) && list.write("[]") >= 0 &&
                list.commit());
        QVERIFY(Seed({}));
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                     ->findChild<QObject*>("sessionsModel")
                                     ->property("ready")
                                     .toBool(),
                                 TIMEOUT_MS);
        Window()->resize(WIDTH, HEIGHT);
        Shot("list-empty-no-sessions");
    }

    void ClaudeCannotList() {
        QVERIFY(WriteScene(BROKEN_CLAUDE));
        QVERIFY(Seed({}));
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!UnavailableNow().isEmpty() &&
                                     PanelOf(LIST)
                                         ->findChild<QObject*>("sessionsModel")
                                         ->property("ready")
                                         .toBool(),
                                 TIMEOUT_MS);
        Window()->resize(WIDTH, HEIGHT);
        Shot("list-agent-unavailable");
    }

    void StoreReadOnly() {
        QVERIFY(WriteScene());
        QVERIFY(QDir().mkpath(Dir_->filePath("config")));
        QFile broken(Dir_->filePath("config/sessions.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly) &&
                broken.write("{not json") > 0);
        broken.close();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Count(), SESSION_COUNT, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(List()->property("readOnly").toBool(),
                                 TIMEOUT_MS);
        Window()->resize(WIDTH, HEIGHT);
        Shot("list-store-read-only-problem");
    }

    void PluginProblems() {
        QVERIFY(WriteScene());
        QVERIFY(Seed(Doc()));
        // Two made-up plugins that cannot load: unreadable manifest, and a
        // panel whose QML file is missing.
        const QDir plugins(Dir_->filePath("plugins"));
        QVERIFY(plugins.mkpath("broken-manifest"));
        QVERIFY(plugins.mkpath("missing-qml"));
        QFile bad(plugins.filePath("broken-manifest/plugin.json"));
        QVERIFY(bad.open(QIODevice::WriteOnly) && bad.write("{ not json") > 0);
        bad.close();
        QFile missing(plugins.filePath("missing-qml/plugin.json"));
        QVERIFY(missing.open(QIODevice::WriteOnly));
        missing.write(R"({"id":"missing-qml","version":"0.1.0","api":"0",
            "contributes":{"panels":[{"id":"view","qml":"qml/Nope.qml",
            "inputs":{},"outputs":{}}]}})");
        missing.close();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Count(), SESSION_COUNT, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!Problems().isEmpty(), TIMEOUT_MS);
        Window()->resize(WIDTH, HEIGHT);
        qInfo() << "problems:" << Problems();
        Shot("window-plugin-problems-strip");
    }

    void NoTerminalPlugin() {
        WithoutTerminal_ = true;
        BootScene(Doc());
        if (QTest::currentTestFailed()) {
            return;
        }
        Shot("window-no-terminal-plugin-placeholder");
    }

    void DaemonGoesAway() {
        BootScene(Doc());
        if (QTest::currentTestFailed()) {
            return;
        }
        MoveToId("bg-a1");
        OpenCurrent("run the tests");
        ToList();
        StopDaemon();
        QTRY_VERIFY_WITH_TIMEOUT(RootText() != QString("connected"),
                                 TIMEOUT_MS);
        Shot("banner-disconnected-panels-kept");
    }

    // The developer window: one TerminalItem and nothing else (roost
    // --session).
    void SingleSessionWindow() {
        QVERIFY(WriteScene());
        QVERIFY(Seed({}));
        QVERIFY(StartDaemon());
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"visible", false}});
        engine.loadFromModule("Roost.Shell", "SessionMain");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window =
            qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window != nullptr);
        window->resize(WIDTH, HEIGHT);
        auto* item = window->findChild<TerminalItem*>("terminal");
        QVERIFY(item != nullptr);
        auto mirror = std::make_unique<Mirror>(window);
        QVERIFY(mirror->Ready());
        SingleSession controller(item, "claude", "bg-a1");
        QSignalSpy opened(&controller, &SingleSession::Opened);
        controller.Start(SocketPath());
        QVERIFY(opened.wait(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(controller.Session()).contains("run the tests"), TIMEOUT_MS);
        QTest::qWait(500);
        const QImage image = mirror->Grab();
        QVERIFY(!image.isNull());
        const QString file =
            QStringLiteral("%1-session-window-roost-session@%2x.png")
                .arg(++Number_, 2, 10, QLatin1Char('0'))
                .arg(qRound(window->devicePixelRatio()));
        QVERIFY(image.save(Out_.filePath(file)));
        Taken_ << file;
        // Mirror, window and controller go before the daemon.
        mirror.reset();
        QuiesceWindow(window);
    }

    // The colours the renderer resolves, as the library gives them.
    void DumpPalette() {
        auto created = VtTerminal::Create(40, 4);
        QVERIFY(created.has_value());
        VtTerminal terminal = std::move(*created);
        QByteArray stream;
        for (int index = 0; index < 16; ++index) {
            const int sgr = index < 8 ? 30 + index : 90 + (index - 8);
            stream += "\x1b[" + QByteArray::number(sgr) + "mX\x1b[0m";
        }
        terminal.Write(stream);
        VtRenderState state;
        QVERIFY(state.BeginUpdate(terminal).has_value());
        state.EndUpdate();
        QString text;
        const auto hex = [](VtRenderState::Rgb c) {
            return QStringLiteral("#%1%2%3")
                .arg(c.R, 2, 16, QLatin1Char('0'))
                .arg(c.G, 2, 16, QLatin1Char('0'))
                .arg(c.B, 2, 16, QLatin1Char('0'));
        };
        const auto cells = state.Rows().first().Cells;
        for (int index = 0; index < 16; ++index) {
            text += QStringLiteral("ansi %1 %2\n")
                        .arg(index)
                        .arg(hex(*cells[index].Foreground));
        }
        text += "default-foreground " + hex(state.DefaultForeground()) + "\n";
        text += "default-background " + hex(state.DefaultBackground()) + "\n";
        QFile file(Out_.filePath("terminal-palette.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(text.toUtf8());
    }

private:
    static void QuiesceAndDrop(QQuickWindow* window);
};

void UiInventory::QuiesceAndDrop(QQuickWindow* window) {
    QuiesceWindow(window);
}

QTEST_MAIN(UiInventory)
#include "ui_inventory.moc"
