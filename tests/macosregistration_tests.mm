#include "macosregistration.h"
#include <QTest>

class MacRegistrationTests : public QObject {
    Q_OBJECT
private slots:
    void aliasesResolveToOneType() {
        QString error;
        const auto types = ik::macAssociationTypes({" .JPG ", "jpeg", "png", "jpg"}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(types.size(), 2);
        QCOMPARE(types[0].toObject()["type"].toString(), QString("public.jpeg"));
        QCOMPARE(types[0].toObject()["extensions"].toArray(), (QJsonArray{"jpg", "jpeg"}));
        QVERIFY(types[0].toObject()["associated_extensions"].toArray().contains("jpg"));
        QVERIFY(types[0].toObject()["associated_extensions"].toArray().contains("jpeg"));
        QCOMPARE(types[1].toObject()["type"].toString(), QString("public.png"));
    }
    void archiveTypesStaySeparateFromImages() {
        QString error;
        const auto types=ik::macAssociationTypes({"zip","tar","png"},&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(types.size(),3);
        QCOMPARE(types[0].toObject()["type"].toString(),QString("public.zip-archive"));
        QCOMPARE(types[1].toObject()["type"].toString(),QString("public.tar-archive"));
        QCOMPARE(types[0].toObject()["kind"].toString(),QString("archive"));
        QCOMPARE(types[2].toObject()["kind"].toString(),QString("image"));
    }
    void unsupportedTypesNeverProduceAnApplicationChange_data() {
        QTest::addColumn<QString>("extension");
        for (const QString &s : {QString("mp4"),QString("app"),QString("../png"),QString("png,jpg"),QString(""),QString("unknownikview")})
            QTest::newRow(qPrintable(s.isEmpty()?QString("empty"):s)) << s;
    }
    void unsupportedTypesNeverProduceAnApplicationChange() {
        QFETCH(QString,extension); QString error;
        QVERIFY(ik::macAssociationTypes({"png",extension}, &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
    void backupRetainsOriginalHandlerAcrossRepeatedRegistration() {
        const QString target="io.ikview.desktop"; QString error;
        const QJsonObject original{{"type","public.png"},{"extensions",QJsonArray{"png"}},
            {"application_path","/System/Applications/Preview.app"},{"application_bundle_identifier","com.apple.Preview"}};
        const auto first=ik::macMergeAssociationBackup({},QJsonArray{original},target,&error);
        QVERIFY2(error.isEmpty(),qPrintable(error)); QCOMPARE(first["entries"].toArray().size(),1);
        auto now=original;now["application_path"]="/Users/example/Applications/ikview.app";now["application_bundle_identifier"]=target;
        QCOMPARE(ik::macMergeAssociationBackup(first,QJsonArray{now},target,&error),first);
        auto external=original;external["application_path"]="/Applications/Other.app";external["application_bundle_identifier"]="example.other";
        QCOMPARE(ik::macMergeAssociationBackup(first,QJsonArray{external},target,&error),first);
        QCOMPARE(first["entries"].toArray()[0].toObject()["application_bundle_identifier"].toString(),QString("com.apple.Preview"));
    }
    void unresolvablePreviousHandlerFailsPreflight() {
        QString error;
        const QJsonObject missing{{"type","public.png"},{"extensions",QJsonArray{"png"}},
            {"application_path",""},{"application_bundle_identifier",""}};
        QVERIFY(ik::macMergeAssociationBackup({},QJsonArray{missing},"io.ikview.desktop",&error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void unrelatedBackupAndDuplicateTypesAreRejected() {
        QString error;
        const QJsonObject prior{{"type","public.png"},{"extensions",QJsonArray{"png"}},
            {"application_path","/System/Applications/Preview.app"},{"application_bundle_identifier","com.apple.Preview"}};
        auto backup=ik::macMergeAssociationBackup({},QJsonArray{prior},"io.ikview.desktop",&error);
        auto wrong=backup;wrong["target_bundle_identifier"]="other.app";
        QVERIFY(ik::macMergeAssociationBackup(wrong,{},"io.ikview.desktop",&error).isEmpty());
        auto duplicate=backup;duplicate["entries"]=QJsonArray{prior,prior};
        QVERIFY(ik::macMergeAssociationBackup(duplicate,{},"io.ikview.desktop",&error).isEmpty());
    }
};
QTEST_GUILESS_MAIN(MacRegistrationTests)
#include "macosregistration_tests.moc"
