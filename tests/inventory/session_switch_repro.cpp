// A repro for "the terminal panel did not redraw when switching sessions".
// Not a test of the product: it is not registered with ctest, and it fails
// when it reproduces the bug. It drives the real window like a user (list
// keys, Return, Ctrl+\) against a real roostd with a fake `claude`, draws the
// window through a QQuickRenderControl with Metal QRhi on the offscreen
// platform (the software scene graph draws TerminalItem wrongly), and after
// every switch grabs frames for about a second. Each frame's terminal area is
// compared with the frame taken when the session was first shown (or with the
// settled frame of the session after its output changed while hidden).
//
// A QQuickRenderControl renders on the GUI thread, like the basic render
// loop. The threaded loop (macOS default) needs a shown window with a swap
// chain, which the offscreen platform cannot give, so it is not covered here.
//
//   REPRO_OUT=<dir> QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=rhi \
//       QML_DISABLE_DISK_CACHE=1 build/dev/bin/session_switch_repro

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
#include "helpers/sessions_window.hpp"
#include "helpers/window_teardown.hpp"
#include "roost/single_session.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// Same as Mirror in ui_inventory.cpp.
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
constexpr int SESSION_COUNT = 6;
// Differing pixels, as a share of the terminal area, below which two frames
// are the same picture.
constexpr double SAME = 0.002;

// Every session paints its own header and its own stripes, so that frames of
// two sessions differ by far more than SAME. A file phase2.<id> next to the
// script makes it repaint (while it is hidden, in the tests that want that).
constexpr const char* SCENE = R"sh(#!/bin/sh
dir=$(dirname "$0"); id=$1; n=${id#bg-a}
paint() {
  r=8
  while [ $r -le 20 ]; do
    w=$(( (n*7 + r*3) % 60 + 6 ))
    printf '\033[%d;1H\033[48;5;%dm%*s\033[0m' $((r+1)) $(( ($1 + n*30) % 200 + 17 )) $w ''
    r=$((r+1))
  done
}
i=0; while [ $i -lt 100 ]; do echo "scrollback $id line $i"; i=$((i+1)); done
printf '\033[?25l\033[1;1H\033[1;37;4%dm SESSION %s \033[0m\033[2;1HPHASE1' $((n%6+1)) $id
paint 0
done2=
while :; do
  if [ -z "$done2" ] && [ -e "$dir/phase2.$id" ]; then
    printf '\033[2;1HPHASE2'
    paint 90
    done2=1
  fi
  sleep 0.1
done
)sh";

constexpr const char* CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) cd / && exec "$dir/scene.sh" "$2" ;;
*) exit 0 ;;
esac
)sh";

QString IdOf(int n) { return QStringLiteral("bg-a%1").arg(n); }

bool WriteExecutable(const QString& path, const char* text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) >= 0 &&
           file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                               QFile::ExeOwner);
}

// The share of pixels that differ by more than a rounding error.
double Diff(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull()) {
        return 1.0;
    }
    qint64 different = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* row_a = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* row_b = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (qAbs(qRed(row_a[x]) - qRed(row_b[x])) > 8 ||
                qAbs(qGreen(row_a[x]) - qGreen(row_b[x])) > 8 ||
                qAbs(qBlue(row_a[x]) - qBlue(row_b[x])) > 8) {
                ++different;
            }
        }
    }
    return static_cast<double>(different) /
           static_cast<double>(a.width() * a.height());
}

}  // namespace

class SessionSwitchRepro : public SessionsWindow {
    Q_OBJECT

public:
    QDir Out_;
    QString Case_;
    int Shots_ = 0;
    QStringList Failures_;
    QMap<QString, QImage> Ref_;
    std::unique_ptr<Mirror> Mirror_;
    QFile Log_;
    // What the render loop does: a frame every 33 ms, whatever the test waits
    // for, so that updatePaintNode runs while a session is still opening.
    QTimer Pump_;

    QQuickWindow* Window() override {
        return Mirror_ ? Mirror_->Window() : SessionsWindow::Window();
    }
    void Adopt(QQuickWindow* root) override {
        Mirror_ = std::make_unique<Mirror>(root);
        QVERIFY(Mirror_->Ready());
    }

    void Say(const QString& line) {
        Log_.write((line + "\n").toUtf8());
        Log_.flush();
        qInfo().noquote() << line;
    }

    bool Boot6() {
        if (!WriteFakeClaude(CLAUDE)) {
            return false;
        }
        QJsonArray agents;
        qint64 at = 1790150000000;
        for (int n = 1; n <= SESSION_COUNT; ++n) {
            agents.append(
                QJsonObject{{"id", IdOf(n)},
                            {"cwd", "/work"},
                            {"kind", "background"},
                            {"startedAt", at += 100000},
                            {"sessionId", QString("0000-%1").arg(IdOf(n))},
                            {"name", QString("session-%1").arg(n)},
                            {"state", "working"}});
        }
        QSaveFile list(AgentsPath());
        if (!list.open(QIODevice::WriteOnly) ||
            list.write(QJsonDocument(agents).toJson()) < 0 || !list.commit() ||
            !WriteExecutable(Dir_->filePath("bin/scene.sh"), SCENE)) {
            return false;
        }
        QDir().mkpath(Dir_->filePath("config"));
        QSaveFile doc(Dir_->filePath("config/sessions.json"));
        if (!doc.open(QIODevice::WriteOnly) ||
            doc.write("{\"version\":1}") < 0 || !doc.commit()) {
            return false;
        }
        return StartDaemon();
    }

    void BootWindow() {
        QVERIFY(Boot6());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Count(), SESSION_COUNT, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                     ->findChild<QObject*>("sessionsModel")
                                     ->property("ready")
                                     .toBool(),
                                 TIMEOUT_MS);
        Window()->resize(WIDTH, HEIGHT);
        QTest::qWait(300);
        connect(&Pump_, &QTimer::timeout, this, [this] {
            if (Mirror_) {
                Mirror_->Grab();
            }
        });
        Pump_.start(33);
    }

    void ToList() {
        QVERIFY(QMetaObject::invokeMethod(Slot(LIST), "forceActiveFocus"));
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
    }

    void MoveToId(const QString& id) {
        for (int step = 0; step < 16 && CursorId() != id; ++step) {
            Press(Qt::Key_Down);
        }
        for (int step = 0; step < 16 && CursorId() != id; ++step) {
            Press(Qt::Key_Up);
        }
        QCOMPARE(CursorId(), id);
    }

    // The terminal area of a frame.
    QImage Area(const QImage& frame) {
        TerminalItem* terminal = Terminal();
        const qreal dpr = Window()->devicePixelRatio();
        const QRectF r = terminal->mapRectToScene(
            QRectF(0, 0, terminal->width(), terminal->height()));
        return frame.copy(QRect(qRound(r.x() * dpr), qRound(r.y() * dpr),
                                qRound(r.width() * dpr),
                                qRound(r.height() * dpr)));
    }

    QImage Frame() {
        QImage frame = Mirror_->Grab();
        return frame.isNull() ? QImage() : Area(frame);
    }

    // Which reference the frame is: a session id, "blank" or "?".
    QString Classify(const QImage& area, const QString& expectRef) {
        if (area.isNull()) {
            return "null";
        }
        if (Ref_.contains(expectRef) && Diff(area, Ref_[expectRef]) < SAME) {
            return expectRef;
        }
        for (auto it = Ref_.cbegin(); it != Ref_.cend(); ++it) {
            if (Diff(area, it.value()) < SAME) {
                return it.key();
            }
        }
        // Blank: one colour all over.
        const QRgb first = area.pixel(0, 0);
        QImage plain(area.size(), area.format());
        plain.fill(first);
        return Diff(area, plain) < SAME ? "blank" : "?";
    }

    void Save(const QImage& area, const QString& tag) {
        area.save(Out_.filePath(QStringLiteral("%1-%2-%3.png")
                                    .arg(Case_)
                                    .arg(++Shots_, 2, 10, QLatin1Char('0'))
                                    .arg(tag)));
    }

    // Waits for the first screen of a session that has not been shown, takes
    // its reference picture.
    void Learn(const QString& id) {
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains("SESSION " + id),
                                 TIMEOUT_MS);
        QTest::qWait(500);
        const QImage area = Frame();
        QVERIFY(!area.isNull());
        Ref_[id] = area;
        Save(area, "ref-" + id);
    }

    // Opens a session with the keys; the first time, learns its picture.
    void Open(const QString& id) {
        ToList();
        MoveToId(id);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), id, TIMEOUT_MS);
        if (!Ref_.contains(id)) {
            Learn(id);
        }
    }

    // Grabs frames for ~1 s after a switch to `id` and judges them. `expect`
    // is the reference the settled frame must equal ("" to compare with the
    // last frame instead, for a screen that changed while hidden).
    void Judge(const QString& what, const QString& id, bool changed = false) {
        QStringList seq;
        QImage first, last;
        QString firstVerdict;
        for (int i = 0; i <= 10; ++i) {
            if (i > 0) {
                QTest::qWait(100);
            }
            const QImage area = Frame();
            const QString verdict = Classify(area, id);
            const bool cells = ScreenNow().contains("SESSION " + id);
            seq << QStringLiteral("%1%2").arg(verdict, cells ? "" : "(cells!)");
            if (i == 0) {
                first = area;
                firstVerdict = verdict;
                Save(area, what + "-t0");
            }
            last = area;
        }
        Save(last, what + "-t1s");
        const QImage before = Ref_.value(id);
        const bool settledOk =
            changed ? (cellsHave("PHASE2") && Diff(last, before) > 0.01 &&
                       Diff(first, last) < SAME)
                    : (Classify(last, id) == id);
        const bool firstOk =
            changed ? Diff(first, last) < SAME : firstVerdict == id;
        Say(QStringLiteral("[%1] %2 -> %3: first=%4 settled=%5 | %6")
                .arg(Case_, what, id, firstOk ? "OK" : "WRONG",
                     settledOk ? "OK" : "WRONG", seq.join(' ')));
        if (changed) {
            Ref_[id] = last;
        }
        if (!firstOk || !settledOk) {
            Failures_ << Case_ + "/" + what;
        }
    }

    bool cellsHave(const QString& text) { return ScreenNow().contains(text); }

    // Switch, then judge.
    void SwitchTo(const QString& id, bool changed = false) {
        ToList();
        MoveToId(id);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), id, TIMEOUT_MS);
        Judge("to-" + id, id, changed);
    }

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
        Out_ = QDir(qEnvironmentVariable(
            "REPRO_OUT", QDir::current().filePath("session-switch-out")));
        QVERIFY(Out_.mkpath("."));
        Log_.setFileName(Out_.filePath("results.txt"));
        QVERIFY(Log_.open(QIODevice::WriteOnly | QIODevice::Truncate));
    }

    void init() {
        Prepare();
        Unexposed_ = true;
        Ref_.clear();
        Failures_.clear();
        Shots_ = 0;
        Case_ = QString::fromLatin1(QTest::currentTestFunction());
    }
    void cleanup() {
        Pump_.stop();
        Pump_.disconnect();
        Mirror_.reset();
        Teardown();
    }

    // A -> B -> C -> A, all three warm.
    void WarmABCA() {
        BootWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        Open("bg-a1");
        Open("bg-a2");
        Open("bg-a3");
        SwitchTo("bg-a1");
        SwitchTo("bg-a2");
        SwitchTo("bg-a3");
        QVERIFY2(Failures_.isEmpty(), qPrintable(Failures_.join(", ")));
    }

    // Six sessions, four warm: the first ones are cold when they come back.
    void ColdBeyondWarmLimit() {
        BootWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        for (int n = 1; n <= SESSION_COUNT; ++n) {
            Open(IdOf(n));
        }
        SwitchTo("bg-a1");  // cold
        SwitchTo("bg-a6");  // warm
        SwitchTo("bg-a2");  // cold
        SwitchTo("bg-a1");  // warm again
        QVERIFY2(Failures_.isEmpty(), qPrintable(Failures_.join(", ")));
    }

    // The output of a hidden session changes before it is shown again.
    void ChangedWhileHidden() {
        BootWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        Open("bg-a1");
        Open("bg-a2");
        Open("bg-a3");
        QVERIFY(QFile(Dir_->filePath("bin/phase2.bg-a2"))
                    .open(QIODevice::WriteOnly));
        QTest::qWait(1500);
        SwitchTo("bg-a2", true);
        SwitchTo("bg-a1");
        SwitchTo("bg-a2");  // phase 2 is in the cells already: new reference
        QVERIFY2(Failures_.isEmpty(), qPrintable(Failures_.join(", ")));
    }

    // After a switch, input must change the picture: the wheel scrolls the
    // session's scrollback. Run on a warm and on a cold target.
    void InputAfterSwitch() {
        BootWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        Open("bg-a1");
        Open("bg-a2");
        QTest::qWait(3000);  // both warm for a while
        const auto wheel = [this](const QString& id) {
            TerminalItem* t = Terminal();
            const QPointF at =
                t->mapToScene(QPointF(t->width() / 2, t->height() / 2));
            const QImage before = Frame();
            for (int i = 0; i < 5; ++i) {
                QWheelEvent e(at, Window()->mapToGlobal(at), QPoint(),
                              QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(Window(), &e);
            }
            QTest::qWait(400);
            const QImage after = Frame();
            Save(after, "wheel-on-" + id);
            const double d = Diff(before, after);
            Say(QStringLiteral("[%1] wheel on %2: %3 of pixels changed")
                    .arg(Case_, id)
                    .arg(d));
            if (d < SAME) {
                Failures_ << Case_ + "/wheel-" + id;
            }
        };
        SwitchTo("bg-a1");  // warm
        wheel("bg-a1");
        SwitchTo("bg-a3");  // never opened
        wheel("bg-a3");
        QVERIFY2(Failures_.isEmpty(), qPrintable(Failures_.join(", ")));
    }

    // Two switches without a frame between them.
    void FastSwitches() {
        BootWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        Open("bg-a1");
        Open("bg-a2");
        Open("bg-a3");
        const auto quick = [this](const QStringList& ids) {
            QVERIFY(QMetaObject::invokeMethod(Slot(LIST), "forceActiveFocus"));
            for (const QString& id : ids) {
                MoveToId(id);
                Press(Qt::Key_Return);
                QVERIFY(
                    QMetaObject::invokeMethod(Slot(LIST), "forceActiveFocus"));
            }
        };
        // A -> B -> C inside one frame, then judge C.
        quick({"bg-a2", "bg-a3"});
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), QString("bg-a3"), TIMEOUT_MS);
        Judge("fast-1-to-3", "bg-a3");
        // C -> A -> B -> A inside one frame, then judge A.
        quick({"bg-a1", "bg-a2", "bg-a1"});
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), QString("bg-a1"), TIMEOUT_MS);
        Judge("fast-3-2-1", "bg-a1");
        // The same with a cold session in the middle.
        for (int n = 4; n <= SESSION_COUNT; ++n) {
            Open(IdOf(n));
        }
        quick({"bg-a6", "bg-a1"});  // a1 is cold by now
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), QString("bg-a1"), TIMEOUT_MS);
        Judge("fast-6-then-cold-1", "bg-a1");
        QVERIFY2(Failures_.isEmpty(), qPrintable(Failures_.join(", ")));
    }
};

QTEST_MAIN(SessionSwitchRepro)
#include "session_switch_repro.moc"
