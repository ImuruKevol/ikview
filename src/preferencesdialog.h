#pragma once

#include <QDialog>
#include <QString>
#include <QVector>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QKeyEvent;
class QSpinBox;
class QTabWidget;

namespace ik {

struct PreferencesValues {
    QString theme = QStringLiteral("dark");
    QString imageFilter = QStringLiteral("soften");
    QString cachePreset = QStringLiteral("auto");
    bool preventSleep = false;
    bool resumeArchive = true;
    bool deleteBookmarkAfterOpen = false;
    bool openContainingFolder = false;
};

struct SystemProfile {
    quint64 memoryBytes = 0;
    int logicalCores = 0;
    QString architecture;
    QString processor;
};

struct CachePreset {
    QString id;
    QString label;
    QString description;
    int megabytes = 256;
};

// Hardware detection is cached; neither opening settings nor changing a preset
// starts a process, scans the filesystem, or queries memory pressure repeatedly.
SystemProfile detectSystemProfile();
QVector<CachePreset> cachePresets(const SystemProfile &profile);
int resolvedCacheMB(const QString &preset, const SystemProfile &profile);
QString cachePresetForLegacyMB(int megabytes, const SystemProfile &profile);

class PreferencesDialog final : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(const PreferencesValues &initial, const SystemProfile &profile,
                      QWidget *shortcutPage, QWidget *parent = nullptr);
    PreferencesValues values() const;
    bool clearArchivePositionsRequested() const;
    void setValidationCallback(std::function<bool(QString *)> callback);

public slots:
    void accept() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    QComboBox *m_theme;
    QComboBox *m_imageFilter;
    QComboBox *m_cache;
    QCheckBox *m_awake;
    QCheckBox *m_resume;
    QCheckBox *m_removeBookmark;
    QCheckBox *m_openFolder;
    QTabWidget *m_tabs;
    QLabel *m_error;
    bool m_clearPositions = false;
    std::function<bool(QString *)> m_validate;
};

}
