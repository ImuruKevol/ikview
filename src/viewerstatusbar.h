#pragma once
#include <QStatusBar>
class QLabel;
class QStackedWidget;
namespace ik {
class ViewerStatusBar final : public QStatusBar {
public:
    explicit ViewerStatusBar(QWidget *parent = nullptr);
    QLabel *detailsLabel() const { return m_details; }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QLabel *m_details;
    QLabel *m_message;
    QStackedWidget *m_stack;
};
}
