#include "viewerstatusbar.h"
#include <QLabel>
#include <QPainter>
#include <QStackedWidget>
#include <QStyleOption>

namespace ik {
namespace {
class StatusLabel final : public QLabel {
public:
    explicit StatusLabel(QWidget *parent = nullptr) : QLabel(parent) { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); setTextFormat(Qt::PlainText); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setPen(palette().color(QPalette::WindowText));
        const auto area = rect().adjusted(10, 0, -10, 0);
        painter.drawText(area, Qt::AlignLeading | Qt::AlignVCenter,
                         fontMetrics().elidedText(text().simplified(), Qt::ElideRight, area.width()));
    }
};
}
ViewerStatusBar::ViewerStatusBar(QWidget *parent) : QStatusBar(parent) {
    setSizeGripEnabled(false);
    m_stack = new QStackedWidget(this); m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_details = new StatusLabel; m_details->setObjectName("statusDetailsLabel"); m_details->setText("이미지를 열어 시작하세요");
    m_message = new StatusLabel; m_message->setObjectName("statusMessageLabel");
    m_stack->addWidget(m_details); m_stack->addWidget(m_message); addPermanentWidget(m_stack, 1);
    connect(this, &QStatusBar::messageChanged, this, [this](const QString &message) {
        m_message->setText(message); m_message->setToolTip(message); m_stack->setCurrentIndex(message.isEmpty() ? 0 : 1);
    });
}
void ViewerStatusBar::paintEvent(QPaintEvent *) {
    // QStatusBar's transient text can overlap restored children after the bar
    // is hidden and shown. One stacked label owns the text in both states.
    QPainter painter(this); QStyleOption option; option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
}
}
