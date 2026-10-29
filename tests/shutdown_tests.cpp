#include "document.h"
#include "imagecanvas.h"
#include "mainwindow.h"
#include "shortcutmanager.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QProcess>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QtTest>

namespace {
int quitProbe(QApplication &app, const QString &source, const QString &target)
{
    app.setQuitOnLastWindowClosed(true);
    ik::MainWindow window;
    auto *document = window.findChild<ik::Document *>();
    auto *quit = window.findChild<QAction *>("quitApplicationAction");
    auto *shortcuts = window.findChild<ik::ShortcutManager *>();
    if (!document || !quit || !shortcuts) return 90;
    bool oldCommand = window.findChild<QAction *>("closeImageAction");
    for (const auto &command : shortcuts->entries())
        oldCommand |= command.id == "closeImageAction" || command.label == "이미지 닫기";
    const bool correctBindings = quit->shortcuts().contains(QKeySequence("Ctrl+W"))
        && quit->shortcuts().contains(QKeySequence("Ctrl+Q"))
        && quit->menuRole() == QAction::QuitRole;
    if (oldCommand || !correctBindings) {
        QTextStream(stderr) << "obsolete_command=" << oldCommand
            << " quit_bindings=" << ik::ShortcutManager::displaySequences(quit->shortcuts())
            << " expected_quit=" << QKeySequence(QKeySequence::Quit).toString(QKeySequence::PortableText)
            << " menu_role=" << int(quit->menuRole()) << '\n';
        return 91;
    }

    bool sent = false, lastWindowClosed = false, aboutToQuit = false;
    QObject::connect(&app, &QGuiApplication::lastWindowClosed, &app, [&] { lastWindowClosed = true; });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] { aboutToQuit = true; });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { app.exit(92); });
    deadline.start(5000);

    QTimer ready;
    ready.setInterval(10);
    QObject::connect(&ready, &QTimer::timeout, &window, [&] {
        if (target != "empty" && (document->isBusy() || document->currentImage().isNull())) return;
        ready.stop();
        if (target == "canvas") {
            auto *canvas = window.findChild<ik::ImageCanvas *>();
            for (auto *native : QGuiApplication::allWindows()) {
                if (auto *surface = qobject_cast<QQuickWindow *>(native);
                    surface && surface->property("ikviewCanvas").value<QObject *>() == canvas) {
                    sent = true;
                    QTest::keyClick(surface, Qt::Key_W, Qt::ControlModifier);
                    return;
                }
            }
            app.exit(93);
            return;
        }
        QWidget *receiver = &window;
        if (target == "list") receiver = window.findChild<QListView *>("fileList");
        else if (target == "search") receiver = window.findChild<QLineEdit *>("fileFilter");
        if (!receiver) { app.exit(94); return; }
        receiver->setFocus();
        sent = true;
#ifdef Q_OS_MACOS
        if (target == "korean") {
            // ANSI W (13) produces ㅈ in the Korean two-set layout.
            QKeyEvent override(QEvent::ShortcutOverride, 0x3148, Qt::ControlModifier, 0, 13, 0);
            QKeyEvent press(QEvent::KeyPress, 0x3148, Qt::ControlModifier, 0, 13, 0);
            QApplication::sendEvent(receiver, &override);
            QApplication::sendEvent(receiver, &press);
            return;
        }
#endif
        QTest::keyClick(receiver, target == "quit-key" ? Qt::Key_Q : Qt::Key_W, Qt::ControlModifier);
    });
    window.show();
    if (target != "empty") window.openPaths({source});
    ready.start();
    const int code = app.exec();
    const QJsonObject result {{"key_sent", sent}, {"last_window_closed", lastWindowClosed},
        {"about_to_quit", aboutToQuit}, {"window_visible", window.isVisible()},
        {"document_count", document->count()}, {"source_exists", QFile::exists(source)}};
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    return code;
}
}

class ShutdownTests : public QObject {
    Q_OBJECT
private slots:
    void shortcutExitsApplication_data()
    {
        QTest::addColumn<QString>("target");
        QTest::addColumn<bool>("legacySettings");
        for (const auto &target : {"window", "list", "search", "canvas", "empty", "quit-key"})
            QTest::newRow(target) << QString::fromLatin1(target) << false;
        QTest::newRow("obsolete-image-close-setting") << QStringLiteral("window") << true;
#ifdef Q_OS_MACOS
        QTest::newRow("korean-command-w") << QStringLiteral("korean") << false;
#endif
    }

    void shortcutExitsApplication()
    {
        QFETCH(QString, target); QFETCH(bool, legacySettings);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto source = directory.filePath("image.png"), preferences = directory.filePath("settings.ini");
        QImage image(48, 32, QImage::Format_RGB32); image.fill(Qt::cyan); QVERIFY(image.save(source));
        QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); const auto bytes = original.readAll(); original.close();
        if (legacySettings) {
            QSettings settings(preferences, QSettings::IniFormat);
            settings.setValue("shortcuts/v1/closeImageAction", QStringList {"Ctrl+W"}); settings.sync();
        }
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("IKVIEW_SETTINGS_PATH", preferences);
        QProcess child; child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {"--quit-probe", source, target});
        QVERIFY2(child.waitForFinished(10000), qPrintable(child.errorString()));
        const auto errors = child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        const auto failure = QByteArray::number(child.exitCode()) + ": " + errors;
        QVERIFY2(child.exitCode() == 0, failure.constData());
        const auto result = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
        QVERIFY(result.value("key_sent").toBool()); QVERIFY(result.value("last_window_closed").toBool());
        QVERIFY(result.value("about_to_quit").toBool()); QVERIFY(!result.value("window_visible").toBool());
        QCOMPARE(result.value("document_count").toInt(-1), target == "empty" ? 0 : 1);
        QVERIFY(result.value("source_exists").toBool());
        QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), bytes);
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == "--quit-probe") return quitProbe(app, arguments[2], arguments[3]);
    app.setQuitOnLastWindowClosed(false);
    ShutdownTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "shutdown_tests.moc"
