#include "systemintegration.h"
#ifndef Q_OS_MACOS
#include <QFileDialog>
#include <QTreeView>
#include <QListView>
namespace ik {
QStringList chooseViewerPaths(const QString &initialDirectory, QWidget *parent) {
    QFileDialog dialog(parent, QStringLiteral("이미지·PDF·폴더 열기"), initialDirectory);
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setFileMode(QFileDialog::ExistingFiles);
    for (auto *view : dialog.findChildren<QAbstractItemView *>()) view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles() : QStringList();
}
bool setDesktopWallpaper(const QString &,QString *error) {
    if(error) *error=QStringLiteral("This integration is available on macOS.");
    return false;
}
void inhibitDisplaySleep(bool) {}
}
#endif
