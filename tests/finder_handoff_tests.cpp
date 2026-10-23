#include "finderhandoff.h"
#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

class FinderHandoffTests : public QObject {
    Q_OBJECT
private slots:
    void unicodeAndLiteralFileNames() {
        const QStringList paths {QStringLiteral("/Users/example/한글 ' $(literal) + 100%&?#.png"),
                                 QStringLiteral("/Volumes/Drive A/line\nbreak.pdf"),
                                 QStringLiteral("/Users/example/literal%2Fname.jpg")};
        // Match Foundation NSURLComponents: every literal percent in a path
        // is encoded, including text that itself looks like an escape (%2F).
        QByteArray encoded("ikview://open?");
        for (const auto &path : paths) {
            if (!encoded.endsWith('?')) encoded.append('&');
            encoded.append("path="); encoded.append(QUrl::toPercentEncoding(path));
        }
        QString error;
        QCOMPARE(ik::pathsFromFinderHandoff(QUrl::fromEncoded(encoded), &error), paths);
        QVERIFY(error.isEmpty());
        QCOMPARE(ik::pathsFromFinderHandoff(QUrl("ikview://open?path=%2Ftmp%2Fa+b%252Fc.png")), QStringList({"/tmp/a+b%2Fc.png"}));
    }
    void stableDeduplicationAndFolder() {
        QUrl url(QStringLiteral("ikview://open?path=/tmp/first.png&path=/tmp/photos&path=/tmp/first.png"));
        QCOMPARE(ik::pathsFromFinderHandoff(url), QStringList({"/tmp/first.png", "/tmp/photos"}));
    }
    void rejectsInvalidRequests_data() {
        QTest::addColumn<QString>("value");
        QTest::newRow("scheme") << "https://open?path=/tmp/image.png";
        QTest::newRow("action") << "ikview://delete?path=/tmp/image.png";
        QTest::newRow("relative") << "ikview://open?path=relative.png";
        QTest::newRow("remote") << "ikview://open?path=https%3A%2F%2Fexample.invalid%2Fimage.png";
        QTest::newRow("file-uri") << "ikview://open?path=file%3A%2F%2F%2Ftmp%2Fimage.png";
        QTest::newRow("network") << "ikview://open?path=%2F%2Fhost%2Fimage.png";
        QTest::newRow("null") << "ikview://open?path=%2Ftmp%2Fa%00.png";
        QTest::newRow("credentials") << "ikview://user@open?path=/tmp/image.png";
        QTest::newRow("port") << "ikview://open:42?path=/tmp/image.png";
        QTest::newRow("fragment") << "ikview://open?path=/tmp/image.png#extra";
        QTest::newRow("path") << "ikview://open/extra?path=/tmp/image.png";
        QTest::newRow("extra-parameter") << "ikview://open?path=/tmp/image.png&command=delete";
        QTest::newRow("empty") << "ikview://open";
    }
    void rejectsInvalidRequests() {
        QFETCH(QString, value); QString error;
        QVERIFY(ik::pathsFromFinderHandoff(QUrl(value), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void boundsRequestSize() {
        QUrl url(QStringLiteral("ikview://open")); QUrlQuery query;
        query.addQueryItem("path", "/tmp/" + QString(1024 * 1024, 'a')); url.setQuery(query);
        QVERIFY(ik::pathsFromFinderHandoff(url).isEmpty());
        query.clear(); for (int i=0;i<10001;++i) query.addQueryItem("path", "/tmp/a"); url.setQuery(query);
        QVERIFY(ik::pathsFromFinderHandoff(url).isEmpty());
    }
};
QTEST_GUILESS_MAIN(FinderHandoffTests)
#include "finder_handoff_tests.moc"
