#pragma once
#include <QString>
#include <QStringList>
class QWidget;
namespace ik {
QStringList chooseViewerPaths(const QString &initialDirectory, QWidget *parent);
bool setDesktopWallpaper(const QString &path, QString *error = nullptr);
void inhibitDisplaySleep(bool enabled);
}
