#pragma once

#include <QToolButton>
#include <QWidgetAction>
#include <QPointer>

namespace ik {

// Keep the icon, label and menu indicator on one baseline on every platform.
// QToolButton still owns actions, menus, keyboard input and accessibility.
class ViewerToolButton final : public QToolButton {
public:
    explicit ViewerToolButton(QWidget *parent = nullptr);
    void setCompact(bool compact);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    struct ContentRects { QRect icon, text, indicator; };
    ContentRects contentRects() const;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    int m_extent = 40;
};

// QToolBar's overflow menu needs a real action, not a defaultWidget-only
// QWidgetAction. The original command retains its shortcuts and ownership.
class ViewerToolbarAction final : public QWidgetAction {
public:
    ViewerToolbarAction(QAction *command, const QString &buttonName,
                        Qt::ToolButtonStyle presentation, QObject *parent);
protected:
    QWidget *createWidget(QWidget *parent) override;
private:
    QPointer<QAction> m_command;
    QString m_buttonName;
    Qt::ToolButtonStyle m_presentation;
    void syncCommand();
};

}
