#pragma once
#include <QRect>
#include <QSize>
#include <QString>
class QPainter;
class QPalette;
namespace ik {
struct PlaceholderLayout { QRect icon, title, detail; };
PlaceholderLayout placeholderLayout(const QSize &size);
void paintViewerPlaceholder(QPainter &painter, const QSize &size, const QPalette &palette,
                            const QString &title, const QString &detail);
}
