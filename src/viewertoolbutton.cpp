#include "viewertoolbutton.h"
#include "viewericons.h"

#include <QFontMetrics>
#include <QFontMetricsF>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolBar>
#include <QMenu>
#include <algorithm>

namespace ik {
namespace {
constexpr int inset = 10;
constexpr int gap = 8;
constexpr int arrowWidth = 10;
}
ViewerToolButton::ViewerToolButton(QWidget *parent) : QToolButton(parent) {
    setAutoRaise(true);
    setIconSize(QSize(24, 24));
    setFixedHeight(m_extent);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
}
void ViewerToolButton::setCompact(bool compact) {
    m_extent = compact ? 32 : 40;
    setIconSize(compact ? QSize(20, 20) : QSize(24, 24));
    setFixedHeight(m_extent);
    updateGeometry();
}
QSize ViewerToolButton::sizeHint() const {
    const bool label = toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !text().isEmpty();
    // Integer font advances can round below the fallback font's actual ink
    // width. Reserve two pixels so a fitting Korean label is never ellipsized.
    const int labelWidth = qCeil(QFontMetricsF(font()).horizontalAdvance(text())) + 2;
    int width = label ? inset * 2 + iconSize().width() + gap + labelWidth : m_extent;
    if (menu()) width += gap + arrowWidth;
    return QSize(width, m_extent);
}
QSize ViewerToolButton::minimumSizeHint() const { return sizeHint(); }
ViewerToolButton::ContentRects ViewerToolButton::contentRects() const {
    ContentRects result;
    const bool label = toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !text().isEmpty();
    const int indicatorSpace = menu() ? gap + arrowWidth : 0;
    const int iconX = label ? inset : (width() - indicatorSpace - iconSize().width()) / 2;
    result.icon = QRect(iconX, (height() - iconSize().height()) / 2, iconSize().width(), iconSize().height());
    if (label) {
        const int start = result.icon.right() + 1 + gap;
        result.text = QRect(start, 0, std::max(0, width() - start - inset - indicatorSpace), height());
    }
    if (menu()) result.indicator = QRect(width() - inset - arrowWidth, (height() - arrowWidth) / 2, arrowWidth, arrowWidth);
    result.icon = QStyle::visualRect(layoutDirection(), rect(), result.icon);
    result.text = QStyle::visualRect(layoutDirection(), rect(), result.text);
    result.indicator = QStyle::visualRect(layoutDirection(), rect(), result.indicator);
    return result;
}
void ViewerToolButton::paintEvent(QPaintEvent *) {
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    const auto state = option.state;
    // Draw only the styled button surface. Native menu indicators otherwise
    // overlap the label or sit below it when stylesheets add button padding.
    option.text.clear(); option.icon = QIcon(); option.features = QStyleOptionToolButton::None;
    option.subControls = QStyle::SC_ToolButton;
    painter.drawComplexControl(QStyle::CC_ToolButton, option);
    const auto areas = contentRects();
    const auto mode = !isEnabled() ? QIcon::Disabled : state.testFlag(QStyle::State_MouseOver) ? QIcon::Active : QIcon::Normal;
    icon().paint(&painter, areas.icon, Qt::AlignCenter, mode, isChecked() ? QIcon::On : QIcon::Off);
    const auto foreground = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText);
    if (!areas.text.isEmpty()) {
        painter.setPen(foreground);
        painter.drawText(areas.text, Qt::AlignVCenter | Qt::AlignLeading | Qt::TextSingleLine,
                         fontMetrics().elidedText(text(), Qt::ElideRight, areas.text.width()));
    }
    if (!areas.indicator.isEmpty()) viewerIcon("chevron-down", foreground).paint(&painter, areas.indicator);
}

ViewerToolbarAction::ViewerToolbarAction(QAction *command, const QString &buttonName,
                                       Qt::ToolButtonStyle presentation, QObject *parent)
    : QWidgetAction(parent), m_command(command), m_buttonName(buttonName), m_presentation(presentation) {
    connect(command, &QAction::changed, this, [this] { syncCommand(); });
    connect(this, &QAction::triggered, command, [command] { command->trigger(); });
    syncCommand();
}
void ViewerToolbarAction::syncCommand() {
    if (!m_command) return;
    setText(m_command->text()); setIconText(m_command->iconText()); setIcon(m_command->icon());
    setToolTip(m_command->toolTip()); setStatusTip(m_command->statusTip());
    setEnabled(m_command->isEnabled()); setVisible(m_command->isVisible());
    setCheckable(m_command->isCheckable()); setChecked(m_command->isChecked()); setMenu(m_command->menu());
}
QWidget *ViewerToolbarAction::createWidget(QWidget *parent) {
    if (!qobject_cast<QToolBar *>(parent) || !m_command) return nullptr;
    auto *button = new ViewerToolButton(parent);
    button->setObjectName(m_buttonName); button->setDefaultAction(m_command);
    button->setToolButtonStyle(m_presentation);
    if (m_command->menu()) { button->setMenu(m_command->menu()); button->setPopupMode(QToolButton::InstantPopup); }
    return button;
}
}
