#pragma once

#include <QStringList>
#include <functional>

namespace ik {
// Keep the order Finder supplied. Open accepts supported images, archives and
// folders; containingFolders accepts existing files and folders of any type.
QStringList selectedServicePaths(const QStringList &paths, bool containingFolders);

// Call on the Qt GUI thread after creating QApplication. Native service calls
// enqueue the callback on that thread, without replacing Qt's app delegate.
void installMacFinderServices(std::function<void(QStringList)> openPathsCallback,
                             std::function<void(QString)> errorCallback = {});
void removeMacFinderServices();

// Lightweight diagnostics for regression checks. Query on the Qt GUI thread.
struct MacFinderServiceActivity {
    quint64 validationJobs = 0;
    quint64 validationsOnGuiThread = 0;
    int activeJobs = 0;
    int pendingRequests = 0;
};
MacFinderServiceActivity macFinderServiceActivity();
}
