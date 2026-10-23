#pragma once
#include <QFileInfo>
namespace ik {
inline bool samePhysicalFile(const QString &source,const QString &target) {
    const QFileInfo a(source),b(target);
    if(a.absoluteFilePath()==b.absoluteFilePath()) return true;
    const QString canonicalA=a.canonicalFilePath(),canonicalB=b.canonicalFilePath();
    return !canonicalA.isEmpty() && !canonicalB.isEmpty() && canonicalA==canonicalB;
}
}
