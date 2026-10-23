#include "viewericons.h"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <algorithm>
#include <utility>

namespace ik {
namespace {

void drawGlyph(QPainter &painter, const QString &symbol, const QColor &color)
{
    constexpr qreal stroke = 1.8;
    const QPen pen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const auto line = [&painter](qreal x1, qreal y1, qreal x2, qreal y2) {
        painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    };
    if (symbol == "sidebar") {
        painter.drawRoundedRect(QRectF(3, 3.5, 18, 17), 2, 2);
        line(9, 3.5, 9, 20.5);
        line(5.6, 8, 6.6, 8);
        line(5.6, 12, 6.6, 12);
        line(5.6, 16, 6.6, 16);
    } else if (symbol == "fit") {
        // A picture inside corner guides distinguishes fit from full screen.
        line(3, 8, 3, 3); line(3, 3, 8, 3);
        line(16, 3, 21, 3); line(21, 3, 21, 8);
        line(3, 16, 3, 21); line(3, 21, 8, 21);
        line(16, 21, 21, 21); line(21, 21, 21, 16);
        painter.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawRoundedRect(QRectF(7, 7, 10, 10), 1, 1);
    } else if (symbol == "1:1" || symbol == "actual") {
        painter.drawRoundedRect(QRectF(3, 3.5, 18, 17), 2, 2);
        painter.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        // Vector numbers avoid small, baseline-dependent platform font glyphs.
        line(6.1, 9, 8, 7.5); line(8, 7.5, 8, 16.5); line(6.3, 16.5, 9.6, 16.5);
        line(14.4, 9, 16.3, 7.5); line(16.3, 7.5, 16.3, 16.5); line(14.6, 16.5, 17.9, 16.5);
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 10), .8, .8);
        painter.drawEllipse(QPointF(12, 14), .8, .8);
    } else if (symbol == "+" || symbol == "−" || symbol == "-" || symbol == "zoom-in" || symbol == "zoom-out") {
        painter.drawEllipse(QRectF(3, 3, 13.5, 13.5));
        line(14.7, 14.7, 21, 21);
        line(6.7, 9.75, 12.8, 9.75);
        if (symbol == "+" || symbol == "zoom-in") line(9.75, 6.7, 9.75, 12.8);
    } else if (symbol == "rotate") {
        // The arrow follows the circular tangent, with an uncluttered center.
        QPainterPath circle;
        circle.moveTo(20, 9.5);
        circle.cubicTo(18.9, 6, 15.8, 4, 12, 4);
        circle.cubicTo(7.6, 4, 4, 7.6, 4, 12);
        circle.cubicTo(4, 16.4, 7.6, 20, 12, 20);
        circle.cubicTo(15, 20, 17.6, 18.5, 19, 16.1);
        painter.drawPath(circle);
        QPainterPath arrow;
        arrow.moveTo(14.5, 9.5); arrow.lineTo(20, 9.5); arrow.lineTo(20, 4);
        painter.drawPath(arrow);
    } else if (symbol == "play") {
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF {QPointF(7.5, 4.5), QPointF(20.5, 12), QPointF(7.5, 19.5)});
    } else if (symbol == "pause") {
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawRoundedRect(QRectF(5.5, 4, 4.5, 16), 1, 1);
        painter.drawRoundedRect(QRectF(14, 4, 4.5, 16), 1, 1);
    } else if (symbol == "stop") {
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawRoundedRect(QRectF(5, 5, 14, 14), 1.6, 1.6);
    } else if (symbol == "fullscreen") {
        line(3, 9, 3, 3); line(3, 3, 9, 3);
        line(15, 3, 21, 3); line(21, 3, 21, 9);
        line(3, 15, 3, 21); line(3, 21, 9, 21);
        line(15, 21, 21, 21); line(21, 21, 21, 15);
    } else if (symbol == "info") {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        line(12, 11, 12, 17);
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 7.5), 1, 1);
    } else if (symbol == "bookmark") {
        QPainterPath bookmark;
        bookmark.moveTo(5.5, 3); bookmark.lineTo(18.5, 3);
        bookmark.lineTo(18.5, 21); bookmark.lineTo(12, 16.5);
        bookmark.lineTo(5.5, 21); bookmark.closeSubpath();
        painter.drawPath(bookmark);
    } else if (symbol == "hide" || symbol == "close") {
        line(5, 5, 19, 19); line(19, 5, 5, 19);
    } else if (symbol == "left" || symbol == "right") {
        const bool left = symbol == "left";
        line(left ? 15 : 9, 4.5, left ? 7.5 : 16.5, 12);
        line(left ? 7.5 : 16.5, 12, left ? 15 : 9, 19.5);
    } else if (symbol == "up" || symbol == "down") {
        const bool up = symbol == "up";
        line(4.5, up ? 15 : 9, 12, up ? 7.5 : 16.5);
        line(12, up ? 7.5 : 16.5, 19.5, up ? 15 : 9);
    } else if (symbol == "chevron-down") {
        line(6.5, 9, 12, 14.5); line(12, 14.5, 17.5, 9);
    } else if (symbol == "folder") {
        QPainterPath folder;
        folder.moveTo(3, 6); folder.quadTo(3, 4.5, 4.5, 4.5);
        folder.lineTo(9, 4.5); folder.lineTo(11, 7.5);
        folder.lineTo(19.5, 7.5); folder.quadTo(21, 7.5, 21, 9);
        folder.lineTo(21, 18); folder.quadTo(21, 19.5, 19.5, 19.5);
        folder.lineTo(4.5, 19.5); folder.quadTo(3, 19.5, 3, 18); folder.closeSubpath();
        painter.drawPath(folder);
        line(3.5, 9, 20.5, 9);
    } else if (symbol == "open") {
        painter.drawRoundedRect(QRectF(3, 3.5, 18, 17), 2, 2);
        painter.drawEllipse(QPointF(8, 8.5), 1.5, 1.5);
        QPainterPath image;
        image.moveTo(3.7, 17.2); image.lineTo(9, 12); image.lineTo(12.5, 15.5);
        image.lineTo(16, 11); image.lineTo(20.3, 16.4);
        painter.drawPath(image);
    }
}

class ViewerIconEngine final : public QIconEngine {
public:
    ViewerIconEngine(QString symbol, QColor foreground) : m_symbol(std::move(symbol)), m_foreground(std::move(foreground)) {}

    QIconEngine *clone() const override { return new ViewerIconEngine(m_symbol, m_foreground); }
    QString key() const override { return QStringLiteral("IKViewVectorIcon"); }
    bool isNull() override { return false; }
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        if (rect.isEmpty()) return;
        if (mode == QIcon::Disabled) {
            // Fade the completed glyph once. Fading individual strokes makes
            // their intersections brighter and changes the apparent shape.
            const qreal dpr = painter->device()->devicePixelRatioF();
            QPixmap layer(QSize(qRound(rect.width() * dpr), qRound(rect.height() * dpr)));
            layer.setDevicePixelRatio(dpr);
            layer.fill(Qt::transparent);
            QPainter layerPainter(&layer);
            paint(&layerPainter, QRect(QPoint(), rect.size()), QIcon::Normal, state);
            layerPainter.end();
            painter->save();
            painter->setOpacity(painter->opacity() * .38);
            painter->drawPixmap(rect.topLeft(), layer);
            painter->restore();
            return;
        }
        painter->save();
        const qreal extent = std::min(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - extent) / 2, rect.y() + (rect.height() - extent) / 2);
        painter->scale(extent / 24, extent / 24);
        drawGlyph(*painter, m_symbol, m_foreground);
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        // Since Qt 6.8 this size is logical; older Qt passes physical pixels.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        const QSize pixelSize(qRound(size.width() * scale), qRound(size.height() * scale));
#else
        const QSize pixelSize = size;
#endif
        QPixmap result = pixmap(pixelSize, mode, state);
        result.setDevicePixelRatio(scale);
        return result;
    }

private:
    QString m_symbol;
    QColor m_foreground;
};

}

QIcon viewerIcon(const QString &symbol, const QColor &foreground)
{
    return QIcon(new ViewerIconEngine(symbol, foreground));
}

}
