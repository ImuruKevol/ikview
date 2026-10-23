#include "shortcutmanager.h"
#include "dialoglayout.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QResizeEvent>
#include <QFocusEvent>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSet>
#ifdef Q_OS_MACOS
#include <Carbon/Carbon.h>
#endif

namespace ik {
namespace {
int normalizedKey(QKeyCombination combination)
{
    auto modifiers = combination.keyboardModifiers() & ~Qt::KeypadModifier & ~Qt::GroupSwitchModifier;
    Qt::Key key = combination.key();
    if (key == Qt::Key_Backtab) { key = Qt::Key_Tab; modifiers |= Qt::ShiftModifier; }
    if (key == Qt::Key_Equal && modifiers.testFlag(Qt::ShiftModifier)) key = Qt::Key_Plus;
    // Qt can report the shifted glyph together with Shift. '+' in a menu and
    // Shift+'+' from a physical keyboard represent the same shortcut.
    switch (key) {
    case Qt::Key_Exclam: case Qt::Key_QuoteDbl: case Qt::Key_NumberSign:
    case Qt::Key_Dollar: case Qt::Key_Percent: case Qt::Key_Ampersand:
    case Qt::Key_ParenLeft: case Qt::Key_ParenRight: case Qt::Key_Asterisk:
    case Qt::Key_Plus: case Qt::Key_Colon: case Qt::Key_Less:
    case Qt::Key_Greater: case Qt::Key_Question: case Qt::Key_At:
    case Qt::Key_AsciiCircum: case Qt::Key_Underscore:
    case Qt::Key_BraceLeft: case Qt::Key_Bar: case Qt::Key_BraceRight:
    case Qt::Key_AsciiTilde: modifiers &= ~Qt::ShiftModifier; break;
    default: break;
    }
    return QKeyCombination(modifiers, key).toCombined();
}

bool usable(const QKeySequence &sequence)
{
    if (sequence.isEmpty() || sequence.count() != 1) return false;
    const auto key = sequence[0].key();
    return key != Qt::Key_unknown && key != Qt::Key_Control && key != Qt::Key_Shift
        && key != Qt::Key_Alt && key != Qt::Key_Meta && key != Qt::Key_AltGr;
}

QList<QKeySequence> uniqueSequences(const QList<QKeySequence> &values)
{
    QSet<int> keys;
    QList<QKeySequence> result;
    for (const auto &sequence : values) {
        if (sequence.isEmpty()) continue;
        const int key = normalizedKey(sequence[0]);
        if (!keys.contains(key)) { keys.insert(key); result.append(sequence); }
    }
    return result;
}

bool plainShortcut(const QKeySequence &sequence)
{
    if (sequence.isEmpty()) return false;
    const auto key = sequence[0].key();
    if (key == Qt::Key_Escape) return false;
    return !(sequence[0].keyboardModifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))
        && !(key >= Qt::Key_F1 && key <= Qt::Key_F35);
}

#ifdef Q_OS_MACOS
struct NativeLatinKey { quint32 virtualKey; Qt::Key key; Qt::Key shiftedKey; };

const NativeLatinKey *nativeLatinKey(const QKeyEvent *event)
{
    // Cocoa can report Korean IME characters (e.g. ㅠ for the B key) in
    // QKeyEvent::key(). Use Apple's documented ANSI virtual-key positions only
    // for non-Latin Unicode events. Latin layouts keep their translated key,
    // and named Qt keys such as arrows never enter this path.
    const int key = event->key();
    if (key <= 0xff || key > 0x10ffff) return nullptr;
    // Zero is a valid virtual key (ANSI A), not an absent-code sentinel. A
    // genuine all-zero native A event is still identified as spontaneous.
    if (!event->nativeVirtualKey() && !event->nativeScanCode()
        && !event->nativeModifiers() && !event->spontaneous()) return nullptr;
    // Public macOS SDK: Carbon/HIToolbox/Events.h, ANSI virtual keycode table.
    static constexpr NativeLatinKey keys[] {
        {kVK_ANSI_A, Qt::Key_A, Qt::Key_A}, {kVK_ANSI_S, Qt::Key_S, Qt::Key_S},
        {kVK_ANSI_D, Qt::Key_D, Qt::Key_D}, {kVK_ANSI_F, Qt::Key_F, Qt::Key_F},
        {kVK_ANSI_H, Qt::Key_H, Qt::Key_H}, {kVK_ANSI_G, Qt::Key_G, Qt::Key_G},
        {kVK_ANSI_Z, Qt::Key_Z, Qt::Key_Z}, {kVK_ANSI_X, Qt::Key_X, Qt::Key_X},
        {kVK_ANSI_C, Qt::Key_C, Qt::Key_C}, {kVK_ANSI_V, Qt::Key_V, Qt::Key_V},
        {kVK_ANSI_B, Qt::Key_B, Qt::Key_B}, {kVK_ANSI_Q, Qt::Key_Q, Qt::Key_Q},
        {kVK_ANSI_W, Qt::Key_W, Qt::Key_W}, {kVK_ANSI_E, Qt::Key_E, Qt::Key_E},
        {kVK_ANSI_R, Qt::Key_R, Qt::Key_R}, {kVK_ANSI_Y, Qt::Key_Y, Qt::Key_Y},
        {kVK_ANSI_T, Qt::Key_T, Qt::Key_T}, {kVK_ANSI_1, Qt::Key_1, Qt::Key_Exclam},
        {kVK_ANSI_2, Qt::Key_2, Qt::Key_At}, {kVK_ANSI_3, Qt::Key_3, Qt::Key_NumberSign},
        {kVK_ANSI_4, Qt::Key_4, Qt::Key_Dollar}, {kVK_ANSI_6, Qt::Key_6, Qt::Key_AsciiCircum},
        {kVK_ANSI_5, Qt::Key_5, Qt::Key_Percent}, {kVK_ANSI_Equal, Qt::Key_Equal, Qt::Key_Plus},
        {kVK_ANSI_9, Qt::Key_9, Qt::Key_ParenLeft}, {kVK_ANSI_7, Qt::Key_7, Qt::Key_Ampersand},
        {kVK_ANSI_Minus, Qt::Key_Minus, Qt::Key_Underscore}, {kVK_ANSI_8, Qt::Key_8, Qt::Key_Asterisk},
        {kVK_ANSI_0, Qt::Key_0, Qt::Key_ParenRight}, {kVK_ANSI_RightBracket, Qt::Key_BracketRight, Qt::Key_BraceRight},
        {kVK_ANSI_O, Qt::Key_O, Qt::Key_O}, {kVK_ANSI_U, Qt::Key_U, Qt::Key_U},
        {kVK_ANSI_LeftBracket, Qt::Key_BracketLeft, Qt::Key_BraceLeft}, {kVK_ANSI_I, Qt::Key_I, Qt::Key_I},
        {kVK_ANSI_P, Qt::Key_P, Qt::Key_P}, {kVK_ANSI_L, Qt::Key_L, Qt::Key_L},
        {kVK_ANSI_J, Qt::Key_J, Qt::Key_J}, {kVK_ANSI_Quote, Qt::Key_Apostrophe, Qt::Key_QuoteDbl},
        {kVK_ANSI_K, Qt::Key_K, Qt::Key_K}, {kVK_ANSI_Semicolon, Qt::Key_Semicolon, Qt::Key_Colon},
        {kVK_ANSI_Backslash, Qt::Key_Backslash, Qt::Key_Bar}, {kVK_ANSI_Comma, Qt::Key_Comma, Qt::Key_Less},
        {kVK_ANSI_Slash, Qt::Key_Slash, Qt::Key_Question}, {kVK_ANSI_N, Qt::Key_N, Qt::Key_N},
        {kVK_ANSI_M, Qt::Key_M, Qt::Key_M}, {kVK_ANSI_Period, Qt::Key_Period, Qt::Key_Greater},
        {kVK_ANSI_Grave, Qt::Key_QuoteLeft, Qt::Key_AsciiTilde}
    };
    for (const auto &candidate : keys) if (candidate.virtualKey == event->nativeVirtualKey()) return &candidate;
    return nullptr;
}
#endif

class ShortcutSequenceEdit final : public QKeySequenceEdit {
public:
    explicit ShortcutSequenceEdit(QWidget *parent = nullptr) : QKeySequenceEdit(parent)
    {
        setAttribute(Qt::WA_InputMethodEnabled, false);
        for (auto *line : findChildren<QLineEdit *>()) line->setAttribute(Qt::WA_InputMethodEnabled, false);
    }
protected:
    void keyPressEvent(QKeyEvent *event) override
    {
#ifdef Q_OS_MACOS
        if (const auto *native = nativeLatinKey(event)) {
            const Qt::Key key = event->modifiers().testFlag(Qt::ShiftModifier) ? native->shiftedKey : native->key;
            const auto normalized = QKeyCombination::fromCombined(normalizedKey(QKeyCombination(event->modifiers(), key)));
            if (!m_nativeReleasePending) {
                m_nativeReleasePending = true;
                m_recordedVirtualKey = event->nativeVirtualKey();
                m_recordedKey = normalized.key();
            }
            // Deliberately omit native codes. QKeySequenceEdit's Shift path
            // otherwise asks the platform mapper to restore the IME character.
            QKeyEvent translated(event->type(), normalized.key(), normalized.keyboardModifiers(),
                                 QString(QChar(ushort(normalized.key()))), event->isAutoRepeat(), event->count());
            translated.setTimestamp(event->timestamp());
            QKeySequenceEdit::keyPressEvent(&translated);
            event->setAccepted(translated.isAccepted());
            return;
        }
#endif
        QKeySequenceEdit::keyPressEvent(event);
    }
    void keyReleaseEvent(QKeyEvent *event) override
    {
#ifdef Q_OS_MACOS
        if (m_nativeReleasePending && event->nativeVirtualKey() == m_recordedVirtualKey) {
            m_nativeReleasePending = false;
            QKeyEvent translated(event->type(), m_recordedKey, event->modifiers(), QString(),
                                 event->isAutoRepeat(), event->count());
            translated.setTimestamp(event->timestamp());
            QKeySequenceEdit::keyReleaseEvent(&translated);
            event->setAccepted(translated.isAccepted());
            return;
        }
#endif
        QKeySequenceEdit::keyReleaseEvent(event);
    }
    void focusOutEvent(QFocusEvent *event) override
    {
#ifdef Q_OS_MACOS
        m_nativeReleasePending = false;
#endif
        QKeySequenceEdit::focusOutEvent(event);
    }
private:
#ifdef Q_OS_MACOS
    bool m_nativeReleasePending = false;
    quint32 m_recordedVirtualKey = 0;
    Qt::Key m_recordedKey = Qt::Key_unknown;
#endif
};

QString settingsKey(const QString &id)
{
    // Percent-encode separators and percent itself; IDs remain reversible and
    // distinct even when a command label/path was used by an earlier build.
    QString encoded = id;
    encoded.replace('%', "%25"); encoded.replace('/', "%2F"); encoded.replace('\\', "%5C");
    return QStringLiteral("shortcuts/v1/") + encoded;
}

QString cleanLabel(QString value)
{
    value.replace("&&", QString(QChar(0x1f)));
    value.remove('&'); value.replace(QChar(0x1f), '&');
    return value;
}

class ShortcutAliasLabel final : public QLabel {
public:
    explicit ShortcutAliasLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
        setWordWrap(false);
    }
    void setFullText(const QString &text)
    {
        m_fullText = text;
        setToolTip(text);
        setAccessibleName(text);
        updateText();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateText();
    }
private:
    QString m_fullText;
    void updateText()
    {
        setText(fontMetrics().elidedText(m_fullText, Qt::ElideRight, contentsRect().width()));
    }
};
}

ShortcutManager::ShortcutManager(QObject *parent) : QObject(parent)
{
    setObjectName(QStringLiteral("shortcutManager"));
}

bool ShortcutManager::addAction(const QString &id, const QString &label, const QString &category,
                                QAction *action, const QList<QKeySequence> &defaults, ShortcutScope scope)
{
    if (id.isEmpty() || !action) return false;
    for (const auto &command : m_commands) if (command.id == id || command.action == action) return false;
    for (const auto &sequence : defaults) if (!sequence.isEmpty() && !usable(sequence)) return false;
    const auto sequences = uniqueSequences(defaults);
    m_commands.append({id, cleanLabel(label), cleanLabel(category), sequences, sequences, scope, action});
    updateActions();
    return true;
}

QVector<ShortcutCommand> ShortcutManager::entries() const { return m_commands; }

ShortcutBindings ShortcutManager::bindings() const
{
    ShortcutBindings result;
    for (const auto &command : m_commands) result.insert(command.id, command.sequences);
    return result;
}

ShortcutBindings ShortcutManager::defaultBindings() const
{
    ShortcutBindings result;
    for (const auto &command : m_commands) result.insert(command.id, command.defaults);
    return result;
}

bool ShortcutManager::validate(const ShortcutBindings &values, QString *error) const
{
    if (error) error->clear();
    QMap<int, QString> owners;
    for (const auto &command : m_commands) {
        const auto sequences = values.value(command.id, command.sequences);
        for (const auto &sequence : sequences) {
            if (!usable(sequence)) {
                if (error) *error = QStringLiteral("‘%1’ 단축키는 한 번의 키 조합으로 지정해 주세요.").arg(command.label);
                return false;
            }
            const int key = normalizedKey(sequence[0]);
            if (owners.contains(key) && owners.value(key) != command.id) {
                QString previous;
                for (const auto &candidate : m_commands) if (candidate.id == owners.value(key)) { previous = candidate.label; break; }
                if (error) *error = QStringLiteral("%1 단축키가 ‘%2’와 ‘%3’에 중복 지정되었습니다. 한쪽을 변경하거나 비워 주세요.")
                    .arg(sequence.toString(QKeySequence::NativeText), previous, command.label);
                return false;
            }
            owners.insert(key, command.id);
        }
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        bool known = false;
        for (const auto &command : m_commands) if (command.id == it.key()) { known = true; break; }
        if (!known) { if (error) *error = QStringLiteral("Unknown shortcut command: %1").arg(it.key()); return false; }
    }
    return true;
}

bool ShortcutManager::applyBindings(const ShortcutBindings &values, QString *error)
{
    if (!validate(values, error)) return false;
    bool changed = false;
    for (auto &command : m_commands) {
        const auto sequences = uniqueSequences(values.value(command.id, command.sequences));
        if (command.sequences != sequences) { command.sequences = sequences; changed = true; }
    }
    updateActions();
    if (changed) emit bindingsChanged();
    return true;
}

void ShortcutManager::load(QSettings &settings)
{
    auto candidate = defaultBindings();
    QSet<QString> overridden;
    for (const auto &command : m_commands) {
        const QString key = settingsKey(command.id);
        if (!settings.contains(key)) continue;
        const QStringList stored = settings.value(key).toStringList();
        QList<QKeySequence> parsed; bool valid = true;
        for (const auto &text : stored) {
            const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
            if (!usable(sequence)) { valid = false; break; }
            parsed.append(sequence);
        }
        if (valid) { candidate[command.id] = uniqueSequences(parsed); overridden.insert(command.id); }
    }
    // Revert conflicting overrides, including both sides of a collision. A
    // valid simultaneous swap survives because validation considers the full
    // persisted map rather than applying overrides one by one.
    for (int pass = 0; pass <= m_commands.size(); ++pass) {
        if (validate(candidate)) { applyBindings(candidate); return; }
        QMap<int, QString> owners; QSet<QString> reject;
        for (const auto &command : m_commands) for (const auto &sequence : candidate.value(command.id)) {
            const int key = normalizedKey(sequence[0]);
            if (owners.contains(key) && owners.value(key) != command.id) {
                if (overridden.contains(command.id)) reject.insert(command.id);
                if (overridden.contains(owners.value(key))) reject.insert(owners.value(key));
            } else owners.insert(key, command.id);
        }
        if (reject.isEmpty()) return; // Invalid defaults are a programmer error.
        for (const auto &id : reject) { overridden.remove(id); candidate[id] = defaultBindings().value(id); }
    }
}

void ShortcutManager::save(QSettings &settings) const
{
    for (const auto &command : m_commands) {
        const QString key = settingsKey(command.id);
        if (command.sequences == command.defaults) { settings.remove(key); continue; }
        QStringList values;
        for (const auto &sequence : command.sequences) values.append(sequence.toString(QKeySequence::PortableText));
        settings.setValue(key, values);
    }
}

bool ShortcutManager::sequenceMatches(const QKeySequence &sequence, const QKeyEvent *event)
{
    if (!event || !usable(sequence)) return false;
    const int expected = normalizedKey(sequence[0]);
    if (expected == normalizedKey(event->keyCombination())) return true;
#ifdef Q_OS_MACOS
    if (const auto *native = nativeLatinKey(event)) {
        const auto modifiers = event->modifiers();
        if (expected == normalizedKey(QKeyCombination(modifiers, native->key))) return true;
        if (modifiers.testFlag(Qt::ShiftModifier)
            && expected == normalizedKey(QKeyCombination(modifiers, native->shiftedKey))) return true;
    }
#endif
    return false;
}

bool ShortcutManager::handleKey(QKeyEvent *event, bool execute, bool allowViewer, bool allowCanvas)
{
    if (!event) return false;
    for (const auto &command : m_commands) {
        auto *action = command.action.data();
        if (!action || !action->isEnabled() || !action->isVisible() || action->isSeparator()) continue;
        if (command.scope != ShortcutScope::Window && !allowViewer) continue;
        if (command.scope == ShortcutScope::Canvas && !allowCanvas) continue;
        for (const auto &sequence : command.sequences) {
            if (!allowViewer && plainShortcut(sequence)) continue;
            if (!sequenceMatches(sequence, event)) continue;
            if (execute && (!event->isAutoRepeat() || action->autoRepeat())) action->trigger();
            return true;
        }
    }
    return false;
}

QString ShortcutManager::displaySequences(const QList<QKeySequence> &sequences)
{
    QStringList labels;
    for (const auto &sequence : sequences) if (!sequence.isEmpty()) labels.append(sequence.toString(QKeySequence::NativeText));
    return labels.join(QStringLiteral(" / "));
}

void ShortcutManager::updateActions()
{
    for (const auto &command : m_commands) {
        if (!command.action) continue;
        command.action->setShortcuts(command.sequences);
        bool plain = false;
        for (const auto &sequence : command.sequences) plain |= plainShortcut(sequence);
        command.action->setShortcutContext(command.scope == ShortcutScope::Window && !plain
            ? Qt::WindowShortcut : Qt::WidgetShortcut);
        const auto keys = displaySequences(command.sequences);
        command.action->setToolTip(command.label + (keys.isEmpty() ? QString() : " (" + keys + ")"));
    }
}

struct ShortcutEditor::Impl {
    ShortcutManager *manager;
    ShortcutBindings staged;
    QVector<ShortcutCommand> commands;
    QTableWidget *table;
    QLineEdit *search;
    QLabel *status;
    QVector<QKeySequenceEdit *> editors;
    QVector<ShortcutAliasLabel *> aliases;

    void updateStatus()
    {
        QString error;
        const bool valid = manager->validate(staged, &error);
        if (status->property("validationError").toBool() != !valid) {
            status->setProperty("validationError", !valid);
            status->style()->unpolish(status);
            status->style()->polish(status);
        }
        status->setText(valid ? QStringLiteral("단축키 칸에서 키를 누르세요. 변경하면 해당 기능의 기존 보조 단축키도 함께 교체됩니다.") : error);
        status->setAccessibleName(valid ? QStringLiteral("단축키 편집 안내") : QStringLiteral("단축키 충돌"));
    }
    void refreshRow(int row)
    {
        const auto sequences = staged.value(commands[row].id);
        QSignalBlocker blocker(editors[row]);
        editors[row]->setKeySequence(sequences.isEmpty() ? QKeySequence() : sequences.first());
        aliases[row]->setFullText(sequences.size() > 1
            ? QStringLiteral("보조: ") + ShortcutManager::displaySequences(sequences.mid(1)) : QString());
        aliases[row]->setVisible(sequences.size() > 1);
        table->setRowHeight(row, sequences.size() > 1 ? 70 : 48);
    }
};

ShortcutEditor::ShortcutEditor(ShortcutManager *manager, QWidget *parent) : QWidget(parent), d(std::make_unique<Impl>())
{
    setObjectName(QStringLiteral("shortcutEditor"));
    d->manager = manager; d->commands = manager->entries(); d->staged = manager->bindings();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(12);
    auto *top = new QHBoxLayout; top->setSpacing(12);
    d->search = new QLineEdit;
    d->search->setObjectName(QStringLiteral("shortcutSearch"));
    d->search->setPlaceholderText(QStringLiteral("기능 또는 단축키 검색"));
    d->search->setClearButtonEnabled(true);
    d->search->setAccessibleName(QStringLiteral("단축키 검색"));
    top->addWidget(d->search, 1);
    auto *reset = new QPushButton(QStringLiteral("전체 기본값"));
    reset->setObjectName(QStringLiteral("resetAllShortcutsButton")); top->addWidget(reset);
    layout->addLayout(top);
    d->table = new QTableWidget(d->commands.size(), 4);
    d->table->setObjectName(QStringLiteral("shortcutTable"));
    d->table->setHorizontalHeaderLabels({QStringLiteral("기능"), QStringLiteral("현재 단축키"), QStringLiteral("기본 단축키"), QStringLiteral("복원")});
    d->table->verticalHeader()->hide();
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setSelectionMode(QAbstractItemView::NoSelection);
    d->table->setAlternatingRowColors(true); d->table->setShowGrid(false);
    d->table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); d->table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    d->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    d->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    d->table->horizontalHeader()->resizeSection(1, 168);
    d->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    d->table->horizontalHeader()->resizeSection(2, 112);
    d->table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed); d->table->horizontalHeader()->resizeSection(3, 80);
    layout->addWidget(d->table, 1);
    d->status = new QLabel;
    d->status->setObjectName(QStringLiteral("shortcutValidationLabel"));
    d->status->setProperty("validationError", false);
    d->status->setWordWrap(true); layout->addWidget(d->status);
    for (int row = 0; row < d->commands.size(); ++row) {
        const auto &command = d->commands[row];
        auto *item = new QTableWidgetItem(command.label);
        item->setToolTip(command.category + " · " + command.label);
        d->table->setItem(row, 0, item);
        auto *cell = new QWidget;
        auto *cellLayout = new QVBoxLayout(cell); cellLayout->setContentsMargins(3, 3, 3, 3); cellLayout->setSpacing(1);
        auto *edit = new ShortcutSequenceEdit;
        edit->setObjectName(QStringLiteral("shortcutBinding_") + command.id);
        edit->setAccessibleName(command.label + QStringLiteral(" 단축키"));
        edit->setMaximumSequenceLength(1);
        edit->setFinishingKeyCombinations({});
        edit->setClearButtonEnabled(true);
        cellLayout->addWidget(edit);
        auto *alias = new ShortcutAliasLabel;
        alias->setObjectName(QStringLiteral("shortcutAliases_") + command.id);
        cellLayout->addWidget(alias);
        d->editors.append(edit); d->aliases.append(alias);
        d->table->setCellWidget(row, 1, cell);
        const auto defaultText = ShortcutManager::displaySequences(command.defaults);
        auto *defaultItem = new QTableWidgetItem(defaultText.isEmpty() ? QStringLiteral("없음") : defaultText);
        defaultItem->setToolTip(defaultItem->text()); d->table->setItem(row, 2, defaultItem);
        auto *restore = new QPushButton(QStringLiteral("복원"));
        restore->setAccessibleName(command.label + QStringLiteral(" 기본값 복원"));
        restore->setProperty("subtle", true);
        auto *restoreCell = new QWidget; auto *restoreLayout = new QHBoxLayout(restoreCell);
        restoreLayout->setContentsMargins(4, 4, 4, 4); restoreLayout->addWidget(restore);
        d->table->setCellWidget(row, 3, restoreCell);
        d->refreshRow(row);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, row](const QKeySequence &sequence) {
            d->staged[d->commands[row].id] = sequence.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{sequence};
            d->aliases[row]->hide(); d->table->setRowHeight(row, 48); d->updateStatus();
        });
        connect(restore, &QPushButton::clicked, this, [this, row] {
            d->staged[d->commands[row].id] = d->commands[row].defaults; d->refreshRow(row); d->updateStatus();
        });
    }
    connect(reset, &QPushButton::clicked, this, [this] {
        d->staged = d->manager->defaultBindings();
        for (int row = 0; row < d->commands.size(); ++row) d->refreshRow(row);
        d->updateStatus();
    });
    connect(d->search, &QLineEdit::textChanged, this, [this](const QString &query) {
        for (int row = 0; row < d->commands.size(); ++row) {
            const auto &command = d->commands[row];
            const QString text = command.label + ' ' + command.category + ' '
                + ShortcutManager::displaySequences(d->staged.value(command.id)) + ' '
                + ShortcutManager::displaySequences(command.defaults);
            d->table->setRowHidden(row, !text.contains(query, Qt::CaseInsensitive));
        }
    });
    d->updateStatus();
}

ShortcutEditor::~ShortcutEditor()
{
    for (auto *child : findChildren<QObject *>()) QObject::disconnect(child, nullptr, this, nullptr);
}
ShortcutBindings ShortcutEditor::bindings() const { return d->staged; }
bool ShortcutEditor::validate(QString *error) const { return d->manager->validate(d->staged, error); }
bool ShortcutEditor::apply(QString *error) { return d->manager->applyBindings(d->staged, error); }

}
