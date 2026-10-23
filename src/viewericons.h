#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace ik {

// All toolbar glyphs share a 24-unit canvas and are painted at the requested
// device pixel ratio. The caller supplies the theme's foreground color.
QIcon viewerIcon(const QString &symbol, const QColor &foreground);

}
