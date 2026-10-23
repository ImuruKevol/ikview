#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace ik {
// Read-only resolution. Each item contains type and all requested aliases.
QJsonArray macAssociationTypes(const QStringList &extensions, QString *error = nullptr);
// Pure backup merge: preserve the first resolved application for each UTI.
QJsonObject macMergeAssociationBackup(const QJsonObject &existing, const QJsonArray &current,
                                    const QString &targetBundleIdentifier, QString *error = nullptr);
// Terminal entry point. Only set-default/restore-defaults change defaults.
// Native requests wait for the macOS completion handler, including consent UI.
int runMacRegistrationCommand(const QString &command, const QStringList &extensions, const QString &backupPath);
}
