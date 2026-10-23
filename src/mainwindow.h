#pragma once
#include <QMainWindow>
#include <memory>
namespace ik {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openPaths(const QStringList &paths);
    // Explicit zero-based selection bypasses saved reading positions.
    void openPathsAt(const QStringList &paths, int imageIndex, int page = 0);
    void openFolderKeepingCurrent(const QString &folder);
signals:
    void fileOperationFinished(bool success, bool cancelled, const QString &error);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
