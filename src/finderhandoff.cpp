#include "finderhandoff.h"
#include <QDir>
#include <QSet>
#include <QUrlQuery>

namespace ik {
QStringList pathsFromFinderHandoff(const QUrl &url, QString *error) {
    if (error) error->clear();
    const auto reject = [error](const QString &message) {
        if (error) *error = message;
        return QStringList();
    };
    if (!url.isValid() || url.scheme() != QStringLiteral("ikview") || url.host() != QStringLiteral("open")
        || (!url.path().isEmpty() && url.path() != QStringLiteral("/")) || !url.userInfo().isEmpty()
        || url.port(-1) != -1 || url.hasFragment())
        return reject(QStringLiteral("지원하지 않는 IKView 열기 링크입니다."));
    if (url.toEncoded().size() > 1024 * 1024)
        return reject(QStringLiteral("열기 요청이 너무 큽니다. 선택한 파일 수를 줄여 주세요."));
    const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if (items.isEmpty() || items.size() > 10000)
        return reject(QStringLiteral("열기 링크에 유효한 파일 목록이 없습니다."));
    QStringList paths;
    QSet<QString> seen;
    for (const auto &item : items) {
        const QString &path = item.second;
        if (item.first != QStringLiteral("path") || !QDir::isAbsolutePath(path) || !path.startsWith('/')
            || path.startsWith(QStringLiteral("//")) || path.contains(QChar(0)))
            return reject(QStringLiteral("열기 링크는 절대 경로의 로컬 파일과 폴더만 지원합니다."));
        if (!seen.contains(path)) { seen.insert(path); paths.append(path); }
    }
    return paths;
}
}
