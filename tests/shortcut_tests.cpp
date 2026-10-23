#include "shortcutmanager.h"

#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QtTest>

class ShortcutTests : public QObject {
    Q_OBJECT
private slots:
    void bareBookmarkRoutesOutsideCanvas()
    {
        ik::ShortcutManager manager;
        QAction bookmark(QStringLiteral("책갈피"), this);
        QVERIFY(manager.addAction("bookmark", "책갈피", "책갈피", &bookmark, {QKeySequence("B")}));
        QSignalSpy triggered(&bookmark, &QAction::triggered);
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_B, Qt::NoModifier);
        QVERIFY(manager.handleKey(&override, false, true));
        QCOMPARE(triggered.size(), 0);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
        QVERIFY(manager.handleKey(&press, true, true));
        QCOMPARE(triggered.size(), 1);
        QVERIFY(!manager.handleKey(&press, true, false));
        QCOMPARE(triggered.size(), 1);
    }

    void disabledAndHiddenActionsDoNotTrigger()
    {
        ik::ShortcutManager manager;
        QAction action(this);
        manager.addAction("bookmark", "책갈피", "책갈피", &action, {QKeySequence("B")});
        QSignalSpy triggered(&action, &QAction::triggered);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
        action.setEnabled(false);
        QVERIFY(!manager.handleKey(&press, true, true));
        action.setEnabled(true); action.setVisible(false);
        QVERIFY(!manager.handleKey(&press, true, true));
        QCOMPARE(triggered.size(), 0);
    }

    void windowAndCanvasScopesProtectEditorsAndFocusNavigation()
    {
        ik::ShortcutManager manager;
        QAction preferences(this), clean(this), escape(this), modifiedViewer(this);
        manager.addAction("preferences", "설정", "앱", &preferences, {QKeySequence("Ctrl+,")}, ik::ShortcutScope::Window);
        manager.addAction("clean", "집중 보기", "보기", &clean, {QKeySequence("Tab")}, ik::ShortcutScope::Canvas);
        manager.addAction("escape", "복귀", "보기", &escape, {QKeySequence("Esc")}, ik::ShortcutScope::Window);
        manager.addAction("viewer", "뷰어", "보기", &modifiedViewer, {QKeySequence("Ctrl+Y")}, ik::ShortcutScope::Viewer);
        QKeyEvent settingKey(QEvent::KeyPress, Qt::Key_Comma, Qt::ControlModifier);
        QVERIFY(manager.handleKey(&settingKey, true, false));
        QKeyEvent tabKey(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QVERIFY(!manager.handleKey(&tabKey, true, true, false));
        QVERIFY(manager.handleKey(&tabKey, true, true, true));
        QVERIFY(!manager.handleKey(&tabKey, true, false, true));
        QKeyEvent escapeKey(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(manager.handleKey(&escapeKey, true, false));
        QKeyEvent viewerKey(QEvent::KeyPress, Qt::Key_Y, Qt::ControlModifier);
        QVERIFY(!manager.handleKey(&viewerKey, true, false));
        auto bindings = manager.bindings(); bindings["preferences"] = {QKeySequence("S")};
        QVERIFY(manager.applyBindings(bindings));
        QKeyEvent bareSetting(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
        QVERIFY(!manager.handleKey(&bareSetting, true, false));
        QVERIFY(manager.handleKey(&bareSetting, true, true));
        QCOMPARE(preferences.shortcutContext(), Qt::WidgetShortcut);
    }

    void reassigningRemovesAllFormerAliases()
    {
        ik::ShortcutManager manager;
        QAction next(this);
        manager.addAction("next", "다음", "탐색", &next, {QKeySequence("Right"), QKeySequence("Space"), QKeySequence("PgDown")});
        auto bindings = manager.bindings(); bindings["next"] = {QKeySequence("N")};
        QVERIFY(manager.applyBindings(bindings));
        QSignalSpy triggered(&next, &QAction::triggered);
        for (const auto key : {Qt::Key_Right, Qt::Key_Space, Qt::Key_PageDown}) {
            QKeyEvent old(QEvent::KeyPress, key, Qt::NoModifier);
            QVERIFY(!manager.handleKey(&old, true, true));
        }
        QKeyEvent changed(QEvent::KeyPress, Qt::Key_N, Qt::NoModifier);
        QVERIFY(manager.handleKey(&changed, true, true));
        QCOMPARE(triggered.size(), 1);
        QCOMPARE(next.shortcuts(), QList<QKeySequence>{QKeySequence("N")});
        QVERIFY(next.toolTip().contains(QKeySequence("N").toString(QKeySequence::NativeText)));
        QVERIFY(!next.toolTip().contains("PgDown"));
    }

    void conflictRejectsWholeTransactionIncludingAliases()
    {
        ik::ShortcutManager manager;
        QAction first(this), second(this);
        manager.addAction("first", "첫째", "탐색", &first, {QKeySequence("Right"), QKeySequence("Space")});
        manager.addAction("second", "둘째", "탐색", &second, {QKeySequence("Left")});
        const auto original = manager.bindings();
        auto bindings = original; bindings["second"] = {QKeySequence("Space")};
        QString error;
        QVERIFY(!manager.applyBindings(bindings, &error));
        QVERIFY(error.contains("첫째") && error.contains("둘째"));
        QCOMPARE(manager.bindings(), original);
        bindings["second"] = {QKeySequence("Ctrl+X, Ctrl+C")};
        QVERIFY(!manager.applyBindings(bindings, &error));
        QCOMPARE(manager.bindings(), original);
    }

    void shiftedSymbolsAndKeypadMatchPhysicalEvents()
    {
        QKeyEvent plus(QEvent::KeyPress, Qt::Key_Plus, Qt::ShiftModifier, "+");
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("+"), &plus));
        QKeyEvent shiftedEquals(QEvent::KeyPress, Qt::Key_Equal, Qt::ShiftModifier, "+");
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("+"), &shiftedEquals));
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::KeypadModifier);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Right"), &right));
        QKeyEvent uppercase(QEvent::KeyPress, Qt::Key_B, Qt::ShiftModifier, "B");
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("B"), &uppercase));
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Shift+B"), &uppercase));
        QKeyEvent backtab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Shift+Tab"), &backtab));
    }

    void saveAndLoadPreserveDisabledAndSwappedBindings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
        QAction first(this), second(this), third(this);
        ik::ShortcutManager manager;
        manager.addAction("first/path", "첫째", "테스트", &first, {QKeySequence("A")});
        manager.addAction("second%2Fpath", "둘째", "테스트", &second, {QKeySequence("B")});
        manager.addAction("third", "셋째", "테스트", &third, {QKeySequence("C")});
        auto swapped = manager.bindings();
        swapped["first/path"] = {QKeySequence("B")};
        swapped["second%2Fpath"] = {QKeySequence("A")};
        swapped["third"] = {};
        QVERIFY(manager.applyBindings(swapped));
        manager.save(settings); settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(manager.applyBindings(manager.defaultBindings()));
        manager.load(settings);
        QCOMPARE(manager.bindings(), swapped);
        QVERIFY(third.shortcuts().isEmpty());
        QVERIFY(manager.applyBindings(manager.defaultBindings()));
        manager.save(settings);
        QVERIFY(!settings.contains("shortcuts/v1/third"));
    }

    void staleAndCorruptStoredBindingsDoNotBreakValidOverrides()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
        QAction first(this), second(this), third(this);
        ik::ShortcutManager manager;
        manager.addAction("first", "첫째", "테스트", &first, {QKeySequence("A")});
        manager.addAction("second", "둘째", "테스트", &second, {QKeySequence("B")});
        manager.addAction("third", "셋째", "테스트", &third, {QKeySequence("C")});
        settings.setValue("shortcuts/v1/first", QStringList{"B"}); // Collides with second default.
        settings.setValue("shortcuts/v1/second", QStringList{"Ctrl+X, Ctrl+C"}); // Unsupported sequence.
        settings.setValue("shortcuts/v1/third", QStringList{"Y"});
        settings.setValue("shortcuts/v1/deleted", QStringList{"A"});
        manager.load(settings);
        QCOMPARE(first.shortcut(), QKeySequence("A"));
        QCOMPARE(second.shortcut(), QKeySequence("B"));
        QCOMPARE(third.shortcut(), QKeySequence("Y"));
    }

    void editorStagesChangesAndClearUntilApply()
    {
        ik::ShortcutManager manager;
        QAction bookmark(this);
        manager.addAction("bookmark", "책갈피 추가", "책갈피", &bookmark, {QKeySequence("B"), QKeySequence("F2")});
        ik::ShortcutEditor editor(&manager);
        auto *edit = editor.findChild<QKeySequenceEdit *>("shortcutBinding_bookmark");
        QVERIFY(edit);
        edit->setKeySequence(QKeySequence("Y"));
        QCOMPARE(bookmark.shortcut(), QKeySequence("B"));
        QVERIFY(editor.validate());
        QVERIFY(editor.apply());
        QCOMPARE(bookmark.shortcuts(), QList<QKeySequence>{QKeySequence("Y")});
        edit->clear();
        QVERIFY(editor.apply());
        QVERIFY(bookmark.shortcuts().isEmpty());
        editor.findChild<QPushButton *>("resetAllShortcutsButton")->click();
        QVERIFY(editor.apply());
        QCOMPARE(bookmark.shortcuts(), QList<QKeySequence>({QKeySequence("B"), QKeySequence("F2")}));
    }

    void autorepeatAndDestroyedTargetsAreSafe()
    {
        ik::ShortcutManager manager;
        auto *bookmark = new QAction(this);
        bookmark->setAutoRepeat(false);
        manager.addAction("bookmark", "책갈피", "책갈피", bookmark, {QKeySequence("B")});
        QSignalSpy triggered(bookmark, &QAction::triggered);
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b", true);
        QVERIFY(manager.handleKey(&repeat, true, true));
        QCOMPARE(triggered.size(), 0);
        delete bookmark;
        QVERIFY(!manager.handleKey(&repeat, true, true));
        auto values = manager.bindings(); values["bookmark"] = {QKeySequence("Y")};
        QVERIFY(manager.applyBindings(values));
    }

    void editorAliasesStaySingleLineAndShowConflicts()
    {
        ik::ShortcutManager manager;
        QAction previous(this), bookmark(this);
        const QList<QKeySequence> aliases {QKeySequence("Left"), QKeySequence("Up"), QKeySequence("PgUp"), QKeySequence("Backspace"), QKeySequence("Shift+Space")};
        manager.addAction("previous_internal_id", "이전 이미지", "탐색", &previous, aliases);
        manager.addAction("bookmark", "책갈피", "책갈피", &bookmark, {QKeySequence("B")});
        ik::ShortcutEditor editor(&manager);
        auto *table = editor.findChild<QTableWidget *>("shortcutTable");
        auto *alias = editor.findChild<QLabel *>("shortcutAliases_previous_internal_id");
        auto *status = editor.findChild<QLabel *>("shortcutValidationLabel");
        QVERIFY(table && alias && status);
        QCOMPARE(table->item(0, 0)->toolTip(), QStringLiteral("탐색 · 이전 이미지"));
        QVERIFY(!alias->wordWrap());
        QCOMPARE(alias->toolTip(), QStringLiteral("보조: ") + ik::ShortcutManager::displaySequences(aliases.mid(1)));
        QCOMPARE(alias->accessibleName(), alias->toolTip());
        QCOMPARE(table->rowHeight(0), 70);
        auto *edit = editor.findChild<QKeySequenceEdit *>("shortcutBinding_bookmark");
        QVERIFY(edit);
        edit->setKeySequence(QKeySequence("Up"));
        QVERIFY(status->property("validationError").toBool());
        QVERIFY(status->text().contains("책갈피") && status->text().contains("이전 이미지"));
        edit->clear();
        QVERIFY(!status->property("validationError").toBool());
    }

#ifdef Q_OS_MACOS
    void nativeKoreanKeysMatchLatinCommands_data()
    {
        QTest::addColumn<int>("unicodeKey");
        QTest::addColumn<quint32>("virtualKey");
        QTest::addColumn<QString>("shortcut");
        QTest::newRow("B-korean-yu") << 0x3160 << quint32(11) << QStringLiteral("B");
        QTest::newRow("I-korean-ya") << 0x3151 << quint32(34) << QStringLiteral("I");
        QTest::newRow("S-korean-nieun") << 0x3134 << quint32(1) << QStringLiteral("S");
        QTest::newRow("U-korean-yeo") << 0x3155 << quint32(32) << QStringLiteral("U");
        QTest::newRow("Y-korean-yo") << 0x315b << quint32(16) << QStringLiteral("Y");
    }

    void nativeKoreanKeysMatchLatinCommands()
    {
        QFETCH(int, unicodeKey); QFETCH(quint32, virtualKey); QFETCH(QString, shortcut);
        QKeyEvent native(QEvent::KeyPress, unicodeKey, Qt::NoModifier, 0, virtualKey, 0);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence(shortcut), &native));
    }

    void nativeImeRebindingRemovesFormerKeyAndProtectsEditors()
    {
        ik::ShortcutManager manager;
        QAction bookmark(this);
        manager.addAction("bookmark", "책갈피", "책갈피", &bookmark, {QKeySequence("B")});
        QSignalSpy triggered(&bookmark, &QAction::triggered);
        QKeyEvent koreanB(QEvent::KeyPress, 0x3160, Qt::NoModifier, 0, 11, 0);
        QVERIFY(manager.handleKey(&koreanB, true, true));
        QCOMPARE(triggered.size(), 1);
        QVERIFY(!manager.handleKey(&koreanB, true, false));
        QCOMPARE(triggered.size(), 1);
        auto bindings = manager.bindings(); bindings["bookmark"] = {QKeySequence("Y")};
        QVERIFY(manager.applyBindings(bindings));
        QVERIFY(!manager.handleKey(&koreanB, true, true));
        QKeyEvent koreanY(QEvent::KeyPress, 0x315b, Qt::NoModifier, 0, 16, 0);
        QVERIFY(manager.handleKey(&koreanY, true, true));
        QCOMPARE(triggered.size(), 2);
        QVERIFY(!manager.handleKey(&koreanY, true, false));
        QCOMPARE(triggered.size(), 2);
    }

    void nativeImeRespectsLatinLayoutsModifiersAndCodeZero()
    {
        QKeyEvent latinQ(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, 0, 11, 0);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Q"), &latinQ));
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("B"), &latinQ));
        QKeyEvent latinExtended(QEvent::KeyPress, Qt::Key_Adiaeresis, Qt::NoModifier, 0, 0, 0x100);
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("A"), &latinExtended));
        QKeyEvent shiftedS(QEvent::KeyPress, 0x3134, Qt::ShiftModifier, 0, 1, 0x20000);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Shift+S"), &shiftedS));
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("S"), &shiftedS));
        QKeyEvent commandB(QEvent::KeyPress, 0x3160, Qt::ControlModifier, 0, 11, 0x100000);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Ctrl+B"), &commandB));
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("B"), &commandB));
        // ANSI A is code zero and native scan code is zero on macOS. Native
        // flags distinguish this constructed event from one with no metadata.
        QKeyEvent nativeA(QEvent::KeyPress, 0x3141, Qt::NoModifier, 0, 0, 0x100);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("A"), &nativeA));
        QKeyEvent missingNative(QEvent::KeyPress, 0x3141, Qt::NoModifier);
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("A"), &missingNative));
        QKeyEvent arrow(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, 0, 11, 0);
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("B"), &arrow));
        QKeyEvent invalidCode(QEvent::KeyPress, 0x3160, Qt::NoModifier, 0, 0xffff, 0);
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("B"), &invalidCode));
    }

    void nativeImeAsciiPunctuationPreservesShift()
    {
        QKeyEvent shiftedOne(QEvent::KeyPress, 0x3160, Qt::ShiftModifier, 0, 18, 0x20000);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("!"), &shiftedOne));
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("Shift+1"), &shiftedOne));
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("1"), &shiftedOne));
        QKeyEvent bracket(QEvent::KeyPress, 0x3160, Qt::NoModifier, 0, 33, 0);
        QVERIFY(ik::ShortcutManager::sequenceMatches(QKeySequence("["), &bracket));
        QVERIFY(!ik::ShortcutManager::sequenceMatches(QKeySequence("{"), &bracket));
    }

    void editorCapturesLatinShortcutFromNativeKoreanKey_data()
    {
        QTest::addColumn<int>("unicodeKey");
        QTest::addColumn<quint32>("virtualKey");
        QTest::addColumn<int>("modifiers");
        QTest::addColumn<QString>("expected");
        QTest::newRow("Y") << 0x315b << quint32(16) << int(Qt::NoModifier) << QStringLiteral("Y");
        QTest::newRow("Command-Y") << 0x315b << quint32(16) << int(Qt::ControlModifier) << QStringLiteral("Ctrl+Y");
        QTest::newRow("Shift-S") << 0x3134 << quint32(1) << int(Qt::ShiftModifier) << QStringLiteral("Shift+S");
        QTest::newRow("Shift-plus") << 0x3160 << quint32(24) << int(Qt::ShiftModifier) << QStringLiteral("+");
    }

    void editorCapturesLatinShortcutFromNativeKoreanKey()
    {
        QFETCH(int, unicodeKey); QFETCH(quint32, virtualKey); QFETCH(int, modifiers); QFETCH(QString, expected);
        ik::ShortcutManager manager;
        QAction bookmark(this);
        manager.addAction("bookmark", "책갈피", "책갈피", &bookmark, {QKeySequence("B")});
        ik::ShortcutEditor editor(&manager);
        auto *edit = editor.findChild<QKeySequenceEdit *>("shortcutBinding_bookmark");
        QVERIFY(edit);
        QVERIFY(!edit->testAttribute(Qt::WA_InputMethodEnabled));
        for (auto *line : edit->findChildren<QLineEdit *>()) QVERIFY(!line->testAttribute(Qt::WA_InputMethodEnabled));
        QKeyEvent press(QEvent::KeyPress, unicodeKey, Qt::KeyboardModifiers(modifiers), 0, virtualKey, 0);
        QKeyEvent release(QEvent::KeyRelease, unicodeKey, Qt::KeyboardModifiers(modifiers), 0, virtualKey, 0);
        QApplication::sendEvent(edit, &press);
        QApplication::sendEvent(edit, &release);
        QCOMPARE(edit->keySequence().toString(QKeySequence::PortableText), expected);
        QCOMPARE(editor.bindings().value("bookmark"), QList<QKeySequence>{QKeySequence(expected)});
        QCOMPARE(bookmark.shortcut(), QKeySequence("B")); // Still staged.
        QVERIFY(editor.apply());
        QCOMPARE(bookmark.shortcut(), QKeySequence(expected));
    }

    void editorKeepsTranslatedLatinLayoutKey()
    {
        ik::ShortcutManager manager;
        QAction bookmark(this);
        manager.addAction("bookmark", "책갈피", "책갈피", &bookmark, {QKeySequence("B")});
        ik::ShortcutEditor editor(&manager);
        auto *edit = editor.findChild<QKeySequenceEdit *>("shortcutBinding_bookmark");
        QVERIFY(edit);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, 0, 11, 0);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier, 0, 11, 0);
        QApplication::sendEvent(edit, &press); QApplication::sendEvent(edit, &release);
        QCOMPARE(edit->keySequence(), QKeySequence("Q"));
        QCOMPARE(editor.bindings().value("bookmark"), QList<QKeySequence>{QKeySequence("Q")});
    }
#endif
};

QTEST_MAIN(ShortcutTests)
#include "shortcut_tests.moc"
