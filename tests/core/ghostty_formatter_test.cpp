// libghostty-vt is built, linked and usable from C++: a terminal parses VT
// input and the formatter reads the screen back as plain text.

#include <QByteArray>
#include <QTest>

#include <ghostty/vt.h>

#include <memory>
#include <string_view>
#include <type_traits>

namespace {

using TerminalPtr = std::unique_ptr<std::remove_pointer_t<GhosttyTerminal>,
                                    decltype(&ghostty_terminal_free)>;
using FormatterPtr = std::unique_ptr<std::remove_pointer_t<GhosttyFormatter>,
                                     decltype(&ghostty_formatter_free)>;

bool AppendTo(void* userdata, const uint8_t* data, size_t len) {
    static_cast<QByteArray*>(userdata)->append(
        reinterpret_cast<const char*>(data), static_cast<qsizetype>(len));
    return true;
}

}  // namespace

class GhosttyFormatterTest : public QObject {
    Q_OBJECT

private slots:
    void PlainTextDropsStyles();
};

void GhosttyFormatterTest::PlainTextDropsStyles() {
    GhosttyTerminal rawTerminal = nullptr;
    QCOMPARE(ghostty_terminal_new(nullptr, &rawTerminal, 80, 24),
             GHOSTTY_SUCCESS);
    const TerminalPtr terminal(rawTerminal, &ghostty_terminal_free);

    constexpr std::string_view input = "hello\r\n\x1b[31mred";
    ghostty_terminal_vt_write(terminal.get(),
                              reinterpret_cast<const uint8_t*>(input.data()),
                              input.size());

    GhosttyFormatter rawFormatter = nullptr;
    QCOMPARE(
        ghostty_formatter_terminal_new(nullptr, &rawFormatter, terminal.get()),
        GHOSTTY_SUCCESS);
    const FormatterPtr formatter(rawFormatter, &ghostty_formatter_free);

    // Plain text with trailing blanks trimmed is the default; set it anyway so
    // a change of defaults in the library shows up here rather than downstream.
    GhosttyFormatterFormat format = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    QCOMPARE(ghostty_formatter_set(formatter.get(), GHOSTTY_FORMATTER_OPT_EMIT,
                                   &format),
             GHOSTTY_SUCCESS);
    bool trim = true;
    QCOMPARE(ghostty_formatter_set(formatter.get(), GHOSTTY_FORMATTER_OPT_TRIM,
                                   &trim),
             GHOSTTY_SUCCESS);

    QByteArray output;
    const GhosttyWriter writer{.write = AppendTo, .userdata = &output};
    QCOMPARE(ghostty_formatter_format(formatter.get(), writer),
             GHOSTTY_SUCCESS);

    QCOMPARE(output, QByteArray("hello\nred"));
}

QTEST_GUILESS_MAIN(GhosttyFormatterTest)
#include "ghostty_formatter_test.moc"
