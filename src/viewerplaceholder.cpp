#include "viewerplaceholder.h"
#include "viewericons.h"
#include <QPainter>
#include <QPalette>
#include <QTextLayout>
#include <algorithm>

namespace ik {
namespace {
void drawWrappedText(QPainter &painter, const QRect &area, const QString &text, bool centerVertically) {
    if (area.isEmpty()) return;
    const QFontMetrics metrics(painter.font());
    const int lineHeight = metrics.height() + 2;
    const int maximumLines = std::max(1, area.height() / lineHeight);
    QString normalized = text; normalized.replace('\n', QChar::LineSeparator);
    QTextLayout layout(normalized, painter.font()); QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(option); layout.beginLayout();
    QVector<QTextLine> lines;
    while (true) { auto line = layout.createLine(); if (!line.isValid()) break; line.setLineWidth(area.width()); lines.append(line); }
    layout.endLayout();
    const int visible = std::min(maximumLines, int(lines.size()));
    int y = area.y() + (centerVertically ? (area.height() - visible * lineHeight) / 2 : 0);
    for (int i = 0; i < visible; ++i) {
        const auto line = lines[i];
        const auto value = i + 1 == visible && visible < lines.size()
            ? metrics.elidedText(normalized.mid(line.textStart()).simplified(), Qt::ElideRight, area.width())
            : normalized.mid(line.textStart(), line.textLength()).trimmed();
        painter.drawText(QPoint(area.center().x() - metrics.horizontalAdvance(value) / 2, y + metrics.ascent()), value);
        y += lineHeight;
    }
}
}
PlaceholderLayout placeholderLayout(const QSize &size) {
    const bool compact = size.width() < 360 || size.height() < 300;
    const int inset = compact ? 12 : 24;
    const int width = std::max(0, std::min(560, size.width() - inset * 2));
    const int left = (size.width() - width) / 2;
    const int titleHeight = compact ? 42 : 54;
    const int detailHeight = std::min(compact ? 54 : 84, std::max(0, size.height() - inset * 2 - titleHeight - 12));
    const int iconHeight = !compact && size.height() >= 340 ? 64 : 0;
    const int block = titleHeight + detailHeight + 12 + (iconHeight ? iconHeight + 20 : 0);
    const int top = std::max(inset, (size.height() - block) / 2);
    PlaceholderLayout result;
    if (iconHeight) result.icon = QRect((size.width() - 64) / 2, top, 64, 64);
    const int titleTop = top + (iconHeight ? iconHeight + 20 : 0);
    result.title = QRect(left, titleTop, width, titleHeight);
    result.detail = QRect(left, titleTop + titleHeight + 12, width, detailHeight);
    return result;
}
void paintViewerPlaceholder(QPainter &painter, const QSize &size, const QPalette &palette,
                            const QString &title, const QString &detail) {
    const auto layout = placeholderLayout(size);
    painter.save(); painter.setClipRect(QRect(QPoint(), size)); painter.setRenderHint(QPainter::Antialiasing);
    if (!layout.icon.isEmpty()) {
        painter.setPen(Qt::NoPen); painter.setBrush(palette.color(QPalette::Highlight)); painter.drawRoundedRect(layout.icon, 16, 16);
        viewerIcon("open", palette.color(QPalette::HighlightedText)).paint(&painter, layout.icon.adjusted(16, 16, -16, -16));
    }
    auto font = painter.font(); font.setPixelSize(size.width() < 360 || size.height() < 300 ? 16 : 22); font.setWeight(QFont::DemiBold); painter.setFont(font);
    painter.setPen(palette.color(QPalette::WindowText));
    drawWrappedText(painter, layout.title, title, true);
    font.setPixelSize(12); font.setWeight(QFont::Normal); painter.setFont(font); painter.setPen(palette.color(QPalette::PlaceholderText));
    drawWrappedText(painter, layout.detail, detail, false);
    painter.restore();
}
}
