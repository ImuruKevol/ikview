#pragma once
#include "document.h"
#include <QFileInfo>
#include <QImageReader>
#include <QMimeData>
#include <QSet>
#include <QUrl>

namespace ik {
// Both the QWidget shell and the separate native image surface use the same
// validation. A text/document drop must not replace the current image session.
inline QStringList imageDropPaths(const QMimeData *mime)
{
    QStringList paths;
    if (!mime || !mime->hasUrls()) return paths;
    QSet<QString> seen;
    for (const auto &url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QFileInfo info(url.toLocalFile());
        if (!info.exists() || (!info.isDir() && !info.isFile())) continue;
        const auto path = info.absoluteFilePath();
        if (!info.isDir() && !Document::isArchivePath(path) && !Document::isImageFile(path)) continue;
        if (!seen.contains(path)) { seen.insert(path); paths.append(path); }
    }
    return paths;
}
}
