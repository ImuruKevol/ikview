#include "preferencesdialog.h"
#include "dialoglayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QSysInfo>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace ik {
namespace {
constexpr quint64 MiB = 1024ull * 1024ull;
constexpr quint64 GiB = 1024ull * MiB;

QString memoryText(quint64 bytes)
{
    if (bytes == 0) return QStringLiteral("메모리 용량 확인 불가");
    const double gib = static_cast<double>(bytes) / GiB;
    return QStringLiteral("메모리 %1 GiB").arg(gib, 0, 'f', gib >= 1. && gib == std::floor(gib) ? 0 : 1);
}

int highCacheMB(const SystemProfile &profile)
{
    // At most 1/16 of physical memory, rounded down to a power of two.
    // This leaves room for the source image, GPU, decoder and other apps.
    const quint64 budget = profile.memoryBytes ? profile.memoryBytes / MiB / 16 : 512;
    int limit = 32;
    while (limit < 2048 && quint64(limit * 2) <= budget) limit *= 2;
    return limit;
}

QLabel *description(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setProperty("tone", "muted");
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return label;
}

QWidget *scrollPage(QWidget *body)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(body);
    return scroll;
}

QGroupBox *group(const QString &title, QVBoxLayout *parent)
{
    auto *box = new QGroupBox(title);
    parent->addWidget(box);
    return box;
}
}

SystemProfile detectSystemProfile()
{
    static const SystemProfile detected = [] {
        SystemProfile profile;
        profile.architecture = QSysInfo::currentCpuArchitecture();
        profile.logicalCores = std::max(1, QThread::idealThreadCount());
#ifdef Q_OS_MACOS
        uint64_t memory = 0;
        size_t size = sizeof(memory);
        if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) profile.memoryBytes = memory;
        char processor[256] = {};
        size = sizeof(processor);
        if (sysctlbyname("machdep.cpu.brand_string", processor, &size, nullptr, 0) == 0)
            profile.processor = QString::fromUtf8(processor, int(std::min(size, sizeof(processor)))).remove(QChar('\0')).trimmed();
#elif defined(Q_OS_WIN)
        MEMORYSTATUSEX memory = {};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) profile.memoryBytes = memory.ullTotalPhys;
#elif defined(Q_OS_UNIX)
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && pageSize > 0 && quint64(pages) <= std::numeric_limits<quint64>::max() / quint64(pageSize))
            profile.memoryBytes = quint64(pages) * quint64(pageSize);
#endif
        return profile;
    }();
    return detected;
}

QVector<CachePreset> cachePresets(const SystemProfile &profile)
{
    const int high = highCacheMB(profile);
    const int balanced = std::max(32, high / 2);
    const int low = std::max(32, high / 4);
    return {
        {QStringLiteral("auto"), QStringLiteral("자동 · 권장 (%1 MiB)").arg(balanced),
         QStringLiteral("이 Mac의 메모리 용량에 맞춰 균형 설정을 사용합니다. 다른 기기로 옮기면 해당 기기에 맞게 조정됩니다."), balanced},
        {QStringLiteral("low"), QStringLiteral("메모리 절약 (%1 MiB)").arg(low),
         QStringLiteral("다른 앱을 함께 사용하거나 메모리 사용량을 줄이고 싶을 때 적합합니다."), low},
        {QStringLiteral("balanced"), QStringLiteral("균형 (%1 MiB)").arg(balanced),
         QStringLiteral("반복 탐색 속도와 메모리 사용량의 균형을 맞춥니다."), balanced},
        {QStringLiteral("high"), QStringLiteral("빠른 반복 탐색 (%1 MiB)").arg(high),
         QStringLiteral("이미 디코딩한 사진을 더 많이 보관합니다. 이미지 한 장을 처음 여는 속도에는 영향을 주지 않습니다."), high}
    };
}

int resolvedCacheMB(const QString &preset, const SystemProfile &profile)
{
    const auto presets = cachePresets(profile);
    for (const auto &choice : presets) if (choice.id == preset) return choice.megabytes;
    return presets.front().megabytes;
}

QString cachePresetForLegacyMB(int megabytes, const SystemProfile &profile)
{
    if (megabytes < 32 || megabytes > 2048) return QStringLiteral("auto");
    const auto presets = cachePresets(profile);
    QString closest = QStringLiteral("auto");
    int difference = std::numeric_limits<int>::max();
    // Prefer auto when the previous limit already equals the recommendation.
    for (const auto &choice : presets) {
        const int current = std::abs(choice.megabytes - megabytes);
        if (current < difference) { difference = current; closest = choice.id; }
    }
    return closest;
}

PreferencesDialog::PreferencesDialog(const PreferencesValues &initial, const SystemProfile &profile,
                                   QWidget *shortcutPage, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("preferencesDialog"));
    setWindowTitle(QStringLiteral("환경설정"));
    resize(800, 660);
    setMinimumSize(640, 480);
    auto *layout = new QVBoxLayout(this);
    configureDialog(this, 800, 660);
    layout->addWidget(dialogHeading("환경설정", "화면, 성능과 단축키를 원하는 방식으로 조정하세요."));
    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("preferencesTabs"));
    layout->addWidget(m_tabs, 1);

    auto *general = new QWidget;
    auto *generalLayout = new QVBoxLayout(general);
    generalLayout->setContentsMargins(0, 8, 10, 8);
    generalLayout->setSpacing(16);
    auto *appearance = group(QStringLiteral("화면 모드"), generalLayout);
    auto *appearanceLayout = new QFormLayout(appearance);
    configureForm(appearanceLayout);
    m_theme = new QComboBox;
    m_theme->setObjectName(QStringLiteral("themeMode"));
    m_theme->setAccessibleName(QStringLiteral("화면 모드"));
    m_theme->addItem(QStringLiteral("다크 모드"), QStringLiteral("dark"));
    m_theme->addItem(QStringLiteral("라이트 모드"), QStringLiteral("light"));
    m_theme->addItem(QStringLiteral("시스템 설정 따르기"), QStringLiteral("system"));
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(initial.theme)));
    appearanceLayout->addRow(QStringLiteral("테마"), m_theme);
    appearanceLayout->addRow(description(QStringLiteral("앱과 이미지 표시 영역에 함께 적용합니다.")));
    m_imageFilter = new QComboBox;
    m_imageFilter->setObjectName(QStringLiteral("imageFilterMode"));
    m_imageFilter->setAccessibleName(QStringLiteral("이미지 필터"));
    m_imageFilter->addItem(QStringLiteral("필터 없음"), QStringLiteral("none"));
    m_imageFilter->addItem(QStringLiteral("부드럽게 보기"), QStringLiteral("soften"));
    m_imageFilter->addItem(QStringLiteral("선명하게 보기"), QStringLiteral("sharpen"));
    m_imageFilter->addItem(QStringLiteral("부드럽게+선명하게 보기"), QStringLiteral("soften-sharpen"));
    const int filterIndex = m_imageFilter->findData(initial.imageFilter);
    m_imageFilter->setCurrentIndex(filterIndex >= 0 ? filterIndex : m_imageFilter->findData(QStringLiteral("soften")));
    appearanceLayout->addRow(QStringLiteral("이미지 필터"), m_imageFilter);
    appearanceLayout->addRow(description(QStringLiteral("선택한 필터는 모든 이미지와 다음 실행에도 적용됩니다. 원본 파일은 변경하지 않습니다.")));

    auto *opening = group(QStringLiteral("파일과 책갈피"), generalLayout);
    auto *openingLayout = new QVBoxLayout(opening);
    openingLayout->setSpacing(10);
    m_openFolder = new QCheckBox(QStringLiteral("이미지 파일을 열 때 같은 폴더도 함께 열기"));
    m_openFolder->setObjectName(QStringLiteral("openContainingFolder"));
    m_openFolder->setChecked(initial.openContainingFolder);
    openingLayout->addWidget(m_openFolder);
    openingLayout->addWidget(description(QStringLiteral("끄면 선택한 파일만 엽니다. 보호된 폴더는 macOS에서 접근 허용이나 폴더 선택을 요청할 수 있습니다.")));
    m_removeBookmark = new QCheckBox(QStringLiteral("성공적으로 연 책갈피 자동 삭제"));
    m_removeBookmark->setObjectName(QStringLiteral("deleteBookmarkAfterOpen"));
    m_removeBookmark->setChecked(initial.deleteBookmarkAfterOpen);
    openingLayout->addWidget(m_removeBookmark);
    openingLayout->addWidget(description(QStringLiteral("이미지와 페이지를 성공적으로 연 뒤 삭제합니다. 열기에 실패한 책갈피는 유지합니다.")));
    m_resume = new QCheckBox(QStringLiteral("압축파일의 마지막 열람 위치 기억"));
    m_resume->setObjectName(QStringLiteral("resumeArchivePosition"));
    m_resume->setChecked(initial.resumeArchive);
    openingLayout->addWidget(m_resume);
    auto *clearPositions = new QPushButton(QStringLiteral("압축파일 열람 위치 기록 지우기"));
    clearPositions->setObjectName(QStringLiteral("clearArchivePositionsButton"));
    openingLayout->addWidget(clearPositions, 0, Qt::AlignLeft);
    connect(clearPositions, &QPushButton::clicked, this, [this, clearPositions] {
        m_clearPositions = true;
        clearPositions->setText(QStringLiteral("저장하면 열람 위치 기록을 지웁니다"));
        clearPositions->setEnabled(false);
    });

    auto *viewing = group(QStringLiteral("감상"), generalLayout);
    auto *viewingLayout = new QFormLayout(viewing);
    configureForm(viewingLayout);
    m_awake = new QCheckBox(QStringLiteral("이미지를 보는 동안 화면 켜 두기"));
    m_awake->setObjectName(QStringLiteral("keepDisplayAwake"));
    m_awake->setChecked(initial.preventSleep);
    viewingLayout->addRow(m_awake);
    generalLayout->addStretch();
    m_tabs->addTab(scrollPage(general), QStringLiteral("일반"));

    auto *performance = new QWidget;
    auto *performanceLayout = new QVBoxLayout(performance);
    performanceLayout->setContentsMargins(0, 8, 10, 8);
    performanceLayout->setSpacing(16);
    auto *hardware = group(QStringLiteral("현재 컴퓨터"), performanceLayout);
    auto *hardwareLayout = new QVBoxLayout(hardware);
    QStringList specifications;
    if (!profile.processor.isEmpty()) specifications.append(profile.processor);
    specifications.append(profile.architecture.isEmpty() ? QStringLiteral("아키텍처 확인 불가") : profile.architecture);
    if (profile.logicalCores > 0) specifications.append(QStringLiteral("논리 코어 %1개").arg(profile.logicalCores));
    auto *hardwareLabel = description(specifications.join(QStringLiteral(" · ")) + QLatin1Char('\n') + memoryText(profile.memoryBytes));
    hardwareLabel->setObjectName(QStringLiteral("hardwareSummary"));
    hardwareLayout->addWidget(hardwareLabel);
    auto *cacheGroup = group(QStringLiteral("디코딩 이미지 캐시"), performanceLayout);
    auto *cacheLayout = new QVBoxLayout(cacheGroup);
    cacheLayout->setSpacing(12);
    m_cache = new QComboBox;
    m_cache->setObjectName(QStringLiteral("imageCachePreset"));
    m_cache->setAccessibleName(QStringLiteral("디코딩 이미지 캐시"));
    const auto presets = cachePresets(profile);
    for (const auto &preset : presets) {
        m_cache->addItem(preset.label, preset.id);
        m_cache->setItemData(m_cache->count() - 1, preset.megabytes, Qt::UserRole + 1);
    }
    m_cache->setCurrentIndex(std::max(0, m_cache->findData(initial.cachePreset)));
    cacheLayout->addWidget(m_cache);
    auto *cacheDescription = description(presets[m_cache->currentIndex()].description);
    cacheDescription->setObjectName(QStringLiteral("cachePresetDescription"));
    cacheLayout->addWidget(cacheDescription);
    connect(m_cache, &QComboBox::currentIndexChanged, this, [presets, cacheDescription](int index) {
        if (index >= 0 && index < presets.size()) cacheDescription->setText(presets[index].description);
    });
    auto *cacheNote = description(QStringLiteral("캐시는 이미 디코딩한 이미지의 재사용 공간입니다. 현재 원본 이미지, 처리 중인 데이터와 GPU 텍스처는 별도로 메모리를 사용하므로 앱 전체 메모리 한도를 의미하지 않습니다.\n\n옵션은 물리 메모리를 기준으로 정하며 현재 앱의 캐시 상한은 2048 MiB입니다."));
    cacheNote->setObjectName(QStringLiteral("cacheBudgetNote"));
    cacheLayout->addWidget(cacheNote);
    performanceLayout->addStretch();
    m_tabs->addTab(scrollPage(performance), QStringLiteral("성능"));

    if (!shortcutPage) shortcutPage = description(QStringLiteral("등록된 단축키가 없습니다."));
    m_tabs->addTab(shortcutPage, QStringLiteral("단축키"));
    m_error = description(QString());
    m_error->setObjectName(QStringLiteral("preferencesValidationError"));
    m_error->hide();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("preferencesButtons"));
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("저장"));
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("savePreferencesButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("취소"));
    layout->addWidget(dialogFooter(buttons, "변경 사항은 저장할 때 적용됩니다."));
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PreferencesDialog::reject);
}

PreferencesValues PreferencesDialog::values() const
{
    PreferencesValues result;
    result.theme = m_theme->currentData().toString();
    result.imageFilter = m_imageFilter->currentData().toString();
    result.cachePreset = m_cache->currentData().toString();
    result.preventSleep = m_awake->isChecked();
    result.resumeArchive = m_resume->isChecked();
    result.deleteBookmarkAfterOpen = m_removeBookmark->isChecked();
    result.openContainingFolder = m_openFolder->isChecked();
    return result;
}

bool PreferencesDialog::clearArchivePositionsRequested() const { return m_clearPositions; }

void PreferencesDialog::setValidationCallback(std::function<bool(QString *)> callback)
{
    m_validate = std::move(callback);
}

void PreferencesDialog::accept()
{
    QString error;
    if (m_validate && !m_validate(&error)) {
        m_error->setText(error.isEmpty() ? QStringLiteral("단축키 설정을 확인해 주세요.") : error);
        const auto *inlineError = m_tabs->widget(2)->findChild<QLabel *>("shortcutValidationLabel");
        const bool alreadyShown = inlineError && inlineError->property("validationError").toBool() && inlineError->text() == m_error->text();
        m_error->setVisible(!alreadyShown);
        m_tabs->setCurrentIndex(2);
        return;
    }
    m_error->hide();
    QDialog::accept();
}

void PreferencesDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const auto *buttons = findChild<QDialogButtonBox *>(QStringLiteral("preferencesButtons"));
        const auto *focused = focusWidget();
        const bool explicitDecision = buttons && (focused == buttons->button(QDialogButtonBox::Save)
            || focused == buttons->button(QDialogButtonBox::Cancel));
        if (!explicitDecision) {
            // A native combo popup can consume Return to choose an item and
            // then forward that same key to the dialog. Selecting a field value
            // must not also activate the dialog's default Save button.
            event->accept();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

}
