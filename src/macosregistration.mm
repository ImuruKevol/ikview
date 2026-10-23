#include "macosregistration.h"
#include "document.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <memory>
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace ik {
namespace {
const QString bundleIdentifier = QStringLiteral("io.ikview.desktop");
NSString *ns(const QString &value) {
    return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(value.utf16()) length:value.size()];
}
QString qt(NSString *value) { return value ? QString::fromUtf8(value.UTF8String) : QString(); }
void fail(QString *error, const QString &message) { if (error) *error = message; }
QString normalizedExtension(QString extension) {
    extension = extension.trimmed().toLower();
    if (extension.startsWith('.')) extension.remove(0, 1);
    return extension;
}
bool supportedExtension(const QString &extension) {
    return Document::supportedExtensions().contains(extension) || Document::isArchivePath("file." + extension);
}
bool validBackup(const QJsonObject &backup, const QString &target, QString *error) {
    if (backup.value("schema").toInt() != 1 || backup.value("target_bundle_identifier").toString() != target
        || !backup.value("entries").isArray()) {
        fail(error, QStringLiteral("Invalid association backup schema or target application.")); return false;
    }
    QSet<QString> types;
    for (const auto &value : backup.value("entries").toArray()) {
        const auto row = value.toObject(); const QString type = row.value("type").toString();
        if (!value.isObject() || type.isEmpty() || types.contains(type) || !row.value("extensions").isArray()
            || row.value("extensions").toArray().isEmpty() || !QFileInfo(row.value("application_path").toString()).isAbsolute()
            || row.value("application_bundle_identifier").toString().isEmpty()
            || row.value("application_bundle_identifier").toString() == target) {
            fail(error, QStringLiteral("Invalid or duplicate association backup entry.")); return false;
        }
        types.insert(type);
    }
    return true;
}
QJsonArray statusForTypes(const QJsonArray &types) {
    QJsonArray result;
    for (const auto &value : types) {
        auto row = value.toObject();
        UTType *type = [UTType typeWithIdentifier:ns(row.value("type").toString())];
        NSURL *application = type ? [[NSWorkspace sharedWorkspace] URLForApplicationToOpenContentType:type] : nil;
        NSBundle *bundle = application ? [NSBundle bundleWithURL:application] : nil;
        row["application_path"] = qt(application.path);
        row["application_bundle_identifier"] = qt(bundle.bundleIdentifier);
        row["is_ikview"] = row.value("application_bundle_identifier").toString() == bundleIdentifier;
        result.append(row);
    }
    return result;
}
QString ownApplication(QString *error) {
    NSBundle *bundle = [NSBundle mainBundle];
    if (qt(bundle.bundleIdentifier) != bundleIdentifier || !qt(bundle.bundlePath).endsWith(".app", Qt::CaseInsensitive)) {
        fail(error, QStringLiteral("Run this command using the executable inside the installed ikview.app bundle.")); return {};
    }
    return qt(bundle.bundlePath);
}
bool registerApplication(const QString &path, QString *error) {
    NSURL *url = [NSURL fileURLWithPath:ns(path)];
    const OSStatus status = LSRegisterURL((CFURLRef)url, true);
    if (status != noErr) { fail(error, QStringLiteral("Launch Services registration failed (%1).").arg(status)); return false; }
    NSUpdateDynamicServices();
    return true;
}
QString setDefault(const QString &applicationPath, const QString &typeIdentifier) {
    UTType *type = [UTType typeWithIdentifier:ns(typeIdentifier)];
    if (!type || type.dynamic) return QStringLiteral("The content type is not declared by macOS or an installed application.");
    struct Completion { bool done = false; QString error; };
    const auto state = std::make_shared<Completion>();
    QEventLoop loop;
    QObject receiver;
    QObject *context = &receiver;
    QEventLoop *eventLoop = &loop;
    [[NSWorkspace sharedWorkspace] setDefaultApplicationAtURL:[NSURL fileURLWithPath:ns(applicationPath)]
        toOpenContentType:type completionHandler:^(NSError *nativeError) {
            const QString message = nativeError ? qt(nativeError.localizedDescription) : QString();
            QMetaObject::invokeMethod(context, [state, message, eventLoop] {
                state->error = message; state->done = true; eventLoop->quit();
            }, Qt::QueuedConnection);
        }];
    // A timeout cannot cancel a native consent request. Wait for explicit
    // completion instead of reporting failure while a later consent can apply.
    while (!state->done) loop.exec();
    if (!state->error.isEmpty()) return state->error;
    const auto expected = qt([NSBundle bundleWithPath:ns(applicationPath)].bundleIdentifier);
    QElapsedTimer propagation; propagation.start();
    do {
        NSURL *resolved = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenContentType:type];
        if (resolved && qt([NSBundle bundleWithURL:resolved].bundleIdentifier) == expected) return {};
        QEventLoop wait; QTimer::singleShot(50, &wait, &QEventLoop::quit); wait.exec();
    } while (propagation.elapsed() < 2000);
    return QStringLiteral("macOS completed the request, but the resolved default application did not match the requested application.");
}
bool loadBackup(const QString &path, QJsonObject *result, QString *error) {
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        fail(error, QStringLiteral("Cannot read association backup: %1").arg(path)); return false;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, QStringLiteral("Association backup is not valid JSON: %1").arg(path)); return false;
    }
    *result = document.object();
    return validBackup(*result, bundleIdentifier, error);
}
bool saveBackup(const QString &path, const QJsonObject &backup, QString *error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        fail(error, QStringLiteral("Cannot create association backup directory.")); return false;
    }
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(backup).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        fail(error, QStringLiteral("Cannot save association backup: %1").arg(path)); return false;
    }
    return true;
}
int output(QJsonObject result, const QString &error = {}, int code = 0) {
    result["ok"] = code == 0;
    if (!error.isEmpty()) { result["error"] = error; QTextStream(stderr) << error << '\n'; }
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
    return code;
}
}

QJsonArray macAssociationTypes(const QStringList &extensions, QString *error) {
    @autoreleasepool {
    if (error) error->clear();
    QJsonArray result; QHash<QString, int> positions;
    static const QRegularExpression syntax(QStringLiteral("^[a-z0-9]{1,16}$"));
    for (const auto &raw : extensions) {
        const auto extension = normalizedExtension(raw);
        if (!syntax.match(extension).hasMatch() || !supportedExtension(extension)) {
            fail(error, QStringLiteral("Unsupported image/PDF/archive extension: %1").arg(raw)); return {};
        }
        UTType *type = [UTType typeWithFilenameExtension:ns(extension)];
        if (!type || type.dynamic || (![type conformsToType:UTTypeImage] && ![type conformsToType:UTTypePDF] && !Document::isArchivePath("file." + extension))) {
            fail(error, QStringLiteral("No declared image/PDF/archive content type for .%1. Register ikview.app first.").arg(extension)); return {};
        }
        const QString identifier = qt(type.identifier);
        if (positions.contains(identifier)) {
            const int index = positions.value(identifier);
            auto row = result[index].toObject(); auto aliases = row["extensions"].toArray();
            if (!aliases.contains(extension)) aliases.append(extension);
            row["extensions"] = aliases; result[index] = row;
        } else {
            positions.insert(identifier, int(result.size()));
            QJsonArray related;
            for (NSString *alias in type.tags[UTTagClassFilenameExtension]) related.append(qt(alias));
            result.append(QJsonObject{{"type", identifier}, {"extensions", QJsonArray{extension}},
                {"associated_extensions", related}, {"kind", [type conformsToType:UTTypeImage] ? "image" : [type conformsToType:UTTypePDF] ? "pdf" : "archive"}});
        }
    }
    return result;
    }
}

QJsonObject macMergeAssociationBackup(const QJsonObject &existing, const QJsonArray &current,
                                    const QString &target, QString *error) {
    if (error) error->clear();
    QJsonObject result = existing.isEmpty()
        ? QJsonObject{{"schema", 1}, {"target_bundle_identifier", target}, {"entries", QJsonArray()}} : existing;
    if (!validBackup(result, target, error)) return {};
    auto entries = result["entries"].toArray(); QSet<QString> recorded;
    for (const auto &value : entries) recorded.insert(value.toObject()["type"].toString());
    for (const auto &value : current) {
        const auto row = value.toObject(); const auto type = row["type"].toString();
        if (recorded.contains(type) || row["application_bundle_identifier"].toString() == target) continue;
        if (type.isEmpty() || !row["extensions"].isArray() || row["extensions"].toArray().isEmpty()
            || !QFileInfo(row["application_path"].toString()).isAbsolute() || row["application_bundle_identifier"].toString().isEmpty()) {
            fail(error, QStringLiteral("No restorable default application for %1; no defaults were changed.").arg(type)); return {};
        }
        auto original = row; original.remove("is_ikview"); entries.append(original); recorded.insert(type);
    }
    result["entries"] = entries;
    return result;
}

int runMacRegistrationCommand(const QString &command, const QStringList &extensions, const QString &requestedBackupPath) {
    @autoreleasepool {
        QJsonObject result{{"command", command}}; QString error;
        if (command != "register" && command != "default-status" && command != "set-default" && command != "restore-defaults")
            return output(result, QStringLiteral("Unknown registration command."), 2);
        const QString backupPath = requestedBackupPath.isEmpty()
            ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("default-apps-backup.json")
            : QFileInfo(requestedBackupPath).absoluteFilePath();
        const QString application = ownApplication(&error);
        if (application.isEmpty()) return output(result, error, 2);
        result["application_path"] = application; result["bundle_identifier"] = bundleIdentifier;
        if (command == "register") {
            if (!registerApplication(application, &error)) return output(result, error, 1);
            return output(result);
        }
        result["backup_path"] = backupPath;
        if (command == "restore-defaults") {
            QJsonObject backup;
            if (!QFileInfo::exists(backupPath)) return output(result, QStringLiteral("Association backup does not exist."), 2);
            if (!loadBackup(backupPath, &backup, &error)) return output(result, error, 2);
            const auto entries = backup["entries"].toArray();
            if (entries.isEmpty()) return output(result, QStringLiteral("Association backup has no previous applications to restore."), 2);
            QJsonArray changed, skipped, failed, toRestore;
            // Validate every saved type and target before applying any restore.
            for (const auto &value : entries) {
                const auto row = value.toObject(); QStringList aliases;
                const auto current = statusForTypes(QJsonArray{row})[0].toObject();
                if (current["application_bundle_identifier"].toString() != bundleIdentifier) { skipped.append(current); continue; }
                for (const auto &alias : row["extensions"].toArray()) aliases.append(alias.toString());
                const auto resolved = macAssociationTypes(aliases, &error);
                if (!error.isEmpty() || resolved.size() != 1 || resolved[0].toObject()["type"] != row["type"])
                    return output(result, error.isEmpty() ? QStringLiteral("Saved content type no longer matches its extensions.") : error, 2);
                NSBundle *prior = [NSBundle bundleWithPath:ns(row["application_path"].toString())];
                if (!prior || qt(prior.bundleIdentifier) != row["application_bundle_identifier"].toString())
                    return output(result, QStringLiteral("Previous default application is missing or changed: %1").arg(row["application_path"].toString()), 2);
                toRestore.append(row);
            }
            for (const auto &value : toRestore) {
                auto row = value.toObject();
                const auto current = statusForTypes(QJsonArray{row})[0].toObject();
                if (current["application_bundle_identifier"].toString() != bundleIdentifier) { skipped.append(current); continue; }
                const QString failure = setDefault(row["application_path"].toString(), row["type"].toString());
                if (failure.isEmpty()) changed.append(row); else { row["error"] = failure; failed.append(row); }
            }
            result["changed"] = changed; result["skipped"] = skipped; result["failed"] = failed;
            return output(result, failed.isEmpty() ? QString() : QStringLiteral("Some default applications could not be restored; inspect failed entries."), failed.isEmpty() ? 0 : 1);
        }
        QStringList requested = extensions;
        if (requested.isEmpty() && command == "default-status") requested = {"jpg", "png", "gif", "webp", "heic", "tiff", "bmp"};
        if (requested.isEmpty()) return output(result, QStringLiteral("Specify the image/PDF/archive extensions to change; no defaults were changed."), 2);
        const auto types = macAssociationTypes(requested, &error);
        if (!error.isEmpty()) return output(result, error, 2);
        const auto status = statusForTypes(types); result["types"] = status;
        if (command == "default-status") return output(result);
        QJsonObject existing;
        if (!loadBackup(backupPath, &existing, &error)) return output(result, error, 2);
        const auto backup = macMergeAssociationBackup(existing, status, bundleIdentifier, &error);
        if (!error.isEmpty()) return output(result, error, 2);
        if (!saveBackup(backupPath, backup, &error) || !registerApplication(application, &error)) return output(result, error, 1);
        QJsonArray changed, unchanged, failed;
        for (const auto &value : status) {
            auto row = value.toObject();
            if (row["is_ikview"].toBool()) { unchanged.append(row); continue; }
            const QString failure = setDefault(application, row["type"].toString());
            if (failure.isEmpty()) changed.append(row); else { row["error"] = failure; failed.append(row); }
        }
        result["changed"] = changed; result["unchanged"] = unchanged; result["failed"] = failed;
        result["types"] = statusForTypes(types);
        return output(result, failed.isEmpty() ? QString() : QStringLiteral("Some default applications could not be changed; inspect failed entries and the backup."), failed.isEmpty() ? 0 : 1);
    }
}
}
