#pragma once

#include <QAction>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <memory>

class QKeyEvent;
class QSettings;

namespace ik {

enum class ShortcutScope { Viewer, Window, Canvas };
using ShortcutBindings = QMap<QString, QList<QKeySequence>>;

struct ShortcutCommand {
    QString id;
    QString label;
    QString category;
    QList<QKeySequence> defaults;
    QList<QKeySequence> sequences;
    ShortcutScope scope = ShortcutScope::Viewer;
    QPointer<QAction> action;
};

// Owns shortcut definitions rather than key event delivery. The main window
// routes its QWidget/QWindow events here, including the native Metal surface.
class ShortcutManager final : public QObject {
    Q_OBJECT
public:
    explicit ShortcutManager(QObject *parent = nullptr);
    bool addAction(const QString &id, const QString &label, const QString &category,
                   QAction *action, const QList<QKeySequence> &defaults,
                   ShortcutScope scope = ShortcutScope::Viewer);
    QVector<ShortcutCommand> entries() const;
    ShortcutBindings bindings() const;
    ShortcutBindings defaultBindings() const;
    bool validate(const ShortcutBindings &bindings, QString *error = nullptr) const;
    bool applyBindings(const ShortcutBindings &bindings, QString *error = nullptr);
    // Invalid/stale overrides are ignored individually. Defaults remain usable.
    void load(QSettings &settings);
    void save(QSettings &settings) const;
    bool handleKey(QKeyEvent *event, bool execute, bool allowViewer, bool allowCanvas = true);
    static bool sequenceMatches(const QKeySequence &sequence, const QKeyEvent *event);
    static QString displaySequences(const QList<QKeySequence> &sequences);

signals:
    void bindingsChanged();

private:
    QVector<ShortcutCommand> m_commands;
    void updateActions();
};

// A staged editor: canceling Preferences never changes the live registry.
class ShortcutEditor final : public QWidget {
    Q_OBJECT
public:
    explicit ShortcutEditor(ShortcutManager *manager, QWidget *parent = nullptr);
    ~ShortcutEditor() override;
    ShortcutBindings bindings() const;
    bool validate(QString *error = nullptr) const;
    bool apply(QString *error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}
