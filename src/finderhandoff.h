#pragma once
#include <QStringList>
#include <QUrl>

namespace ik {
// The only accepted deep-link operation opens absolute local paths. It never
// runs a command, changes settings, deletes files, or fetches remote content.
QStringList pathsFromFinderHandoff(const QUrl &url, QString *error = nullptr);
}
