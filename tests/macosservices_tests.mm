#include "macosservices.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest>
#include <QtConcurrent/QtConcurrentRun>
#include <unistd.h>
#import <AppKit/AppKit.h>

@protocol IKViewTestServiceProvider
- (void)openInIKView:(NSPasteboard *)pasteboard userData:(NSString *)userData error:(NSString **)error;
@end

// Hold this app-owned pool without sleeping or blocking the GUI, so tests can
// deterministically replace pending requests before their validation starts.
class PoolGate {
    QSemaphore entered;
    QSemaphore released;
    QFuture<void> work;
public:
    explicit PoolGate(QThreadPool *pool) {
        work = QtConcurrent::run(pool, [this] { entered.release(); released.acquire(); });
    }
    ~PoolGate() { released.release(); work.waitForFinished(); }
    bool ready() { return entered.tryAcquire(1, 3000); }
    void unblock() { released.release(); }
};

class MacOSServicesTests : public QObject {
    Q_OBJECT
    QTemporaryDir temp;

    QString image(const QString &name) {
        const QString path = temp.filePath(name);
        QImage pixels(6, 4, QImage::Format_RGB32); pixels.fill(Qt::green);
        return pixels.save(path, "PNG") ? path : QString();
    }
    static void invoke(NSPasteboard *pasteboard, NSString *mode, NSString **error) {
        [(id<IKViewTestServiceProvider>)[NSApp servicesProvider] openInIKView:pasteboard userData:mode error:error];
    }
private slots:
    void initTestCase() {
        QVERIFY(temp.isValid());
        [NSApplication sharedApplication];
    }
    void cleanup() {
        ik::removeMacFinderServices();
        QTRY_COMPARE_WITH_TIMEOUT(ik::macFinderServiceActivity().activeJobs, 0, 10000);
        QCOMPARE(ik::macFinderServiceActivity().pendingRequests, 0);
    }

    void selectedImagesArchivesFoldersPreserveOrder() {
        const QString photo = image("공백 ' $(literal).PNG"); QVERIFY(!photo.isEmpty());
        const QString archive = temp.filePath("pictures.CBZ"); QFile zip(archive); QVERIFY(zip.open(QIODevice::WriteOnly)); zip.write("archive"); zip.close();
        const QString folder = temp.filePath("folder"); QVERIFY(QDir().mkpath(folder));
        const QString text = temp.filePath("notes.txt"); QFile note(text); QVERIFY(note.open(QIODevice::WriteOnly)); note.write("hello"); note.close();
        QCOMPARE(ik::selectedServicePaths({photo, text, archive, photo, folder, temp.filePath("missing.png"), {}}, false), QStringList({photo, archive, folder}));
    }

    void containingFoldersAcceptsOtherFilesAndDeduplicates() {
        const QString folder = temp.filePath("containing"); QVERIFY(QDir().mkpath(folder));
        const QString text = QDir(folder).filePath("README.txt"); QFile note(text); QVERIFY(note.open(QIODevice::WriteOnly)); note.write("hello"); note.close();
        const QString photo = image("containing-photo.png"); QVERIFY(!photo.isEmpty());
        QCOMPARE(ik::selectedServicePaths({text, folder, photo, text}, true), QStringList({folder, temp.path()}));
    }

    void privatePasteboardQueuesPathsOnGuiThread() {
        const QString photo = image("서비스 공백\nimage.png"); QVERIFY(!photo.isEmpty());
        int calls = 0; QStringList received; QThread *callbackThread = nullptr;
        ik::installMacFinderServices([&](QStringList paths) { ++calls; received = std::move(paths); callbackThread = QThread::currentThread(); });
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        QVERIFY([pasteboard writeObjects:@[[NSURL fileURLWithPath:photo.toNSString()]]]);
        NSString *error = nil; invoke(pasteboard, @"open", &error);
        QVERIFY(!error); QCOMPARE(calls, 0);
        QTRY_COMPARE(calls, 1); QCOMPARE(received.size(), 1);
        // NSURL may return the filesystem's decomposed Unicode spelling.
        // Compare canonical text here without rewriting production paths,
        // then prove that the returned path identifies the actual image.
        QCOMPARE(received.first().normalized(QString::NormalizationForm_C), photo.normalized(QString::NormalizationForm_C));
        QVERIFY(QFileInfo::exists(received.first()));
        const QImage loaded(received.first());
        QCOMPARE(loaded.size(), QSize(6, 4)); QCOMPARE(loaded.pixelColor(2, 2), QColor(Qt::green));
        QCOMPARE(callbackThread, qApp->thread());
        [pasteboard releaseGlobally];
    }

    void legacyFilenamePasteboardStillOpensContainingFolder() {
        const QString photo = image("legacy.png"); QVERIFY(!photo.isEmpty());
        QStringList received;
        ik::installMacFinderServices([&](QStringList paths) { received = std::move(paths); });
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
        QVERIFY([pasteboard setPropertyList:@[photo.toNSString()] forType:NSFilenamesPboardType]);
#pragma clang diagnostic pop
        NSString *error = nil; invoke(pasteboard, @"containing-folder", &error);
        QVERIFY(!error); QTRY_COMPARE(received, QStringList({temp.path()}));
        [pasteboard releaseGlobally];
    }

    void nonFileUrlsAndInvalidRequestsReportErrors() {
        int calls = 0;
        ik::installMacFinderServices([&](QStringList) { ++calls; });
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        QVERIFY([pasteboard writeObjects:@[[NSURL URLWithString:@"https://example.invalid/picture.png"]]]);
        NSString *error = nil; invoke(pasteboard, @"open", &error); QVERIFY(error);
        error = nil; invoke(pasteboard, @"invalid", &error); QVERIFY(error);
        QCoreApplication::processEvents(); QCOMPARE(calls, 0);
        [pasteboard releaseGlobally];
    }

    void removedProviderCancelsAlreadyQueuedRequest() {
        const QString photo = image("cancelled.png"); QVERIFY(!photo.isEmpty());
        int calls = 0;
        id previous = [NSApp servicesProvider];
        ik::installMacFinderServices([&](QStringList) { ++calls; });
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        QVERIFY([pasteboard writeObjects:@[[NSURL fileURLWithPath:photo.toNSString()]]]);
        NSString *error = nil; invoke(pasteboard, @"open", &error); QVERIFY(!error);
        ik::removeMacFinderServices();
        QVERIFY([NSApp servicesProvider] == previous);
        QTRY_COMPARE_WITH_TIMEOUT(ik::macFinderServiceActivity().activeJobs, 0, 10000);
        QCOMPARE(calls, 0);
        [pasteboard releaseGlobally];
    }

    void reinstallUsesLatestCallbackAndRestoresPreviousProvider() {
        const QString photo = image("reinstall.png"); QVERIFY(!photo.isEmpty());
        id previous = [NSApp servicesProvider];
        int oldCalls = 0, newCalls = 0;
        ik::installMacFinderServices([&](QStringList) { ++oldCalls; });
        auto *pool = qApp->findChild<QThreadPool *>(QStringLiteral("ikviewFinderServicePool")); QVERIFY(pool);
        PoolGate gate(pool); QVERIFY(gate.ready());
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        QVERIFY([pasteboard writeObjects:@[[NSURL fileURLWithPath:photo.toNSString()]]]);
        NSString *error = nil; invoke(pasteboard, @"open", &error); QVERIFY(!error);
        ik::installMacFinderServices([&](QStringList) { ++newCalls; });
        invoke(pasteboard, @"open", &error); QVERIFY(!error);
        QCOMPARE(ik::macFinderServiceActivity().activeJobs, 1);
        QCOMPARE(ik::macFinderServiceActivity().pendingRequests, 1);
        gate.unblock();
        QTRY_COMPARE(newCalls, 1); QCOMPARE(oldCalls, 0);
        ik::removeMacFinderServices(); QVERIFY([NSApp servicesProvider] == previous);
        [pasteboard releaseGlobally];
    }

    void invalidLocalSelectionReportsAsynchronouslyOnGuiThread() {
        int opened = 0, errors = 0; QString message; QThread *errorThread = nullptr;
        ik::installMacFinderServices([&](QStringList) { ++opened; }, [&](QString error) {
            ++errors; message = std::move(error); errorThread = QThread::currentThread();
        });
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        QVERIFY([pasteboard writeObjects:@[[NSURL fileURLWithPath:temp.filePath("no-such-file.png").toNSString() isDirectory:NO]]]);
        NSString *error = nil; invoke(pasteboard, @"open", &error);
        QVERIFY(!error); QCOMPARE(errors, 0); QCOMPARE(opened, 0);
        QTRY_COMPARE(errors, 1); QVERIFY(!message.isEmpty()); QCOMPARE(errorThread, qApp->thread()); QCOMPARE(opened, 0);
        [pasteboard releaseGlobally];
    }

    void rapidRequestsKeepOnlyOneActiveAndLatestPending() {
        const QString first = image("first-request.png"), second = image("second-request.png"), third = image("third-request.png");
        QVERIFY(!first.isEmpty() && !second.isEmpty() && !third.isEmpty());
        int calls = 0; QStringList received;
        ik::installMacFinderServices([&](QStringList paths) { ++calls; received = std::move(paths); });
        auto *pool = qApp->findChild<QThreadPool *>(QStringLiteral("ikviewFinderServicePool")); QVERIFY(pool);
        QCOMPARE(pool->maxThreadCount(), 1); QCOMPARE(pool->expiryTimeout(), 10000);
        PoolGate gate(pool); QVERIFY(gate.ready());
        const auto before = ik::macFinderServiceActivity();
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        for (const auto &path : {first, second, third}) {
            [pasteboard clearContents];
            QVERIFY([pasteboard writeObjects:@[[NSURL fileURLWithPath:path.toNSString()]]]);
            NSString *error = nil; invoke(pasteboard, @"open", &error); QVERIFY(!error);
        }
        const auto queued = ik::macFinderServiceActivity();
        QCOMPARE(queued.activeJobs, 1); QCOMPARE(queued.pendingRequests, 1); QCOMPARE(calls, 0);
        gate.unblock();
        QTRY_COMPARE(calls, 1); QCOMPARE(received, QStringList({third}));
        const auto after = ik::macFinderServiceActivity();
        QCOMPARE(after.validationJobs - before.validationJobs, quint64(2));
        QCOMPARE(after.validationsOnGuiThread - before.validationsOnGuiThread, quint64(0));
        QCOMPARE(after.pendingRequests, 0);
        [pasteboard releaseGlobally];
    }

    void largeSelectionsAreValidatedOutsideGuiThread() {
        const QString base = image("large-service-source.png"); QVERIFY(!base.isEmpty());
        const QString folder = temp.filePath("large-service"); QVERIFY(QDir().mkpath(folder));
        QStringList paths; NSMutableArray *urls = [NSMutableArray arrayWithCapacity:4000];
        for (int i = 0; i < 4000; ++i) {
            const auto path = QDir(folder).filePath(QString("%1.png").arg(i, 4, 10, QChar('0')));
            QVERIFY(::link(QFile::encodeName(base).constData(), QFile::encodeName(path).constData()) == 0);
            paths.append(path); [urls addObject:[NSURL fileURLWithPath:path.toNSString() isDirectory:NO]];
        }
        int calls = 0; QStringList received;
        ik::installMacFinderServices([&](QStringList selected) { ++calls; received = std::move(selected); });
        const auto before = ik::macFinderServiceActivity();
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        QVERIFY([pasteboard writeObjects:urls]);
        QElapsedTimer call; call.start();
        NSString *error = nil; invoke(pasteboard, @"open", &error);
        qInfo("4000 Finder URLs: service enqueue returned in %.3f ms", call.nsecsElapsed() / 1e6);
        QVERIFY(!error); QCOMPARE(calls, 0);
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 10000);
        QCOMPARE(received.size(), paths.size()); QCOMPARE(received.first(), paths.first()); QCOMPARE(received.last(), paths.last());
        const auto after = ik::macFinderServiceActivity();
        QCOMPARE(after.validationJobs - before.validationJobs, quint64(1));
        QCOMPARE(after.validationsOnGuiThread - before.validationsOnGuiThread, quint64(0));
        [pasteboard releaseGlobally];
    }
};

QTEST_MAIN(MacOSServicesTests)
#include "macosservices_tests.moc"
