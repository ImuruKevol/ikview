#pragma once
#include <QColor>
#include <QString>
#include <QPalette>
class QStyle;
namespace ik {
QStyle *createViewerStyle();
QPalette viewerPalette(bool dark);
bool applicationThemeIsDark(const QString &theme);
void applyApplicationTheme(const QString &theme);
QColor viewerThemeBackground(const QString &theme);
}
