#include "apptheme.h"
#include <QApplication>
#include <QPalette>
#include <QStyleHints>
#include <QResource>
#include <QProxyStyle>
#include <QDialogButtonBox>

static void initializeViewerStyleResources() { Q_INIT_RESOURCE(viewerstyle); }

namespace ik {
namespace {
using ThemeTokens = QList<QPair<QString, QColor>>;
ThemeTokens themeTokens(bool dark) {
    const auto color = [dark](const char *night, const char *day) { return QColor(dark ? night : day); };
    return {
        {"@window@", color("#151b22", "#f4f6f8")}, {"@surface@", color("#1b242e", "#ffffff")},
        {"@raised@", color("#25313d", "#eaf0f4")}, {"@input@", color("#121920", "#ffffff")},
        {"@hover@", color("#2a3846", "#e5edf3")}, {"@border@", color("#3b4b5b", "#c7d3dd")},
        {"@controlBorder@", color("#586f83", "#7d92a4")},
        {"@text@", color("#dce5ee", "#243543")}, {"@muted@", color("#a1afbd", "#536a7c")},
        {"@accent@", color("#6bd8c1", "#147d68")}, {"@selected@", color("#22433f", "#dff2ec")},
        {"@selectedText@", color("#9cf0da", "#11644f")}, {"@focus@", color("#67bdaa", "#218e76")},
        {"@danger@", color("#ffb3a7", "#a7352b")}, {"@dangerSurface@", color("#3b2a29", "#fff0ec")}
    };

}
QPalette paletteForTokens(const ThemeTokens &tokens) {
    const auto value = [&tokens](const QString &name) { for (const auto &token : tokens) if (token.first == name) return token.second; return QColor(); };
    QPalette palette;
    palette.setColor(QPalette::Window, value("@window@"));
    palette.setColor(QPalette::WindowText, value("@text@"));
    palette.setColor(QPalette::Base, value("@input@"));
    palette.setColor(QPalette::AlternateBase, value("@surface@"));
    palette.setColor(QPalette::Text, value("@text@"));
    palette.setColor(QPalette::PlaceholderText, value("@muted@"));
    palette.setColor(QPalette::Button, value("@raised@"));
    palette.setColor(QPalette::ButtonText, value("@text@"));
    palette.setColor(QPalette::Highlight, value("@selected@"));
    palette.setColor(QPalette::HighlightedText, value("@selectedText@"));
    palette.setColor(QPalette::ToolTipBase, value("@surface@"));
    palette.setColor(QPalette::ToolTipText, value("@text@"));
    palette.setColor(QPalette::Link, value("@accent@"));
    palette.setColor(QPalette::Mid, value("@border@"));
    palette.setColor(QPalette::Dark, value("@border@"));
    for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, value("@muted@"));
    return palette;

}
class ViewerStyle final : public QProxyStyle {
public:
    ViewerStyle() : QProxyStyle(QStringLiteral("Fusion")) {}
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *result) const override {
#ifdef Q_OS_MACOS
        if (hint == QStyle::SH_DialogButtonLayout) return QDialogButtonBox::MacLayout;
#endif
        return QProxyStyle::styleHint(hint, option, widget, result);
    }
};
}
QStyle *createViewerStyle() { return new ViewerStyle; }
QPalette viewerPalette(bool dark) {
    static const auto night = paletteForTokens(themeTokens(true));
    static const auto day = paletteForTokens(themeTokens(false));
    return dark ? night : day;
}
bool applicationThemeIsDark(const QString &theme) {
    return theme == "dark" || (theme == "system" && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}
QColor viewerThemeBackground(const QString &theme) { return QColor(applicationThemeIsDark(theme) ? "#10151c" : "#e8edf2"); }
void applyApplicationTheme(const QString &theme) {
    static const bool resourcesReady = [] { initializeViewerStyleResources(); return true; }();
    Q_UNUSED(resourcesReady);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(theme == "system" ? Qt::ColorScheme::Unknown
        : theme == "light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark);
#endif
    const bool dark = applicationThemeIsDark(theme);
    const auto tokens = themeTokens(dark);
    qApp->setPalette(viewerPalette(dark));
    QString stylesheet = QStringLiteral(R"(
        QWidget { color: @text@; }
        QMainWindow, QDialog { background: @window@; }
        QLabel, QCheckBox, QDialogButtonBox { background: transparent; }
        QLabel#dialogTitle { font-size: 20px; font-weight: 600; }
        QLabel#dialogDescription, QLabel#dialogFooterHint, QLabel[tone="muted"],
        QLabel#secondaryNote, QLabel#hardwareSummary, QLabel#cacheBudgetNote,
        QLabel#shortcutValidationLabel { color: @muted@; }
        QLabel#dialogDescription, QLabel#dialogFooterHint, QLabel#secondaryNote { font-size: 12px; }
        QLabel[tone="error"], QLabel#preferencesValidationError,
        QLabel#shortcutValidationLabel[validationError="true"] { color: @danger@; }
        QLabel[tone="info"] { color: @selectedText@; }
        QWidget#dialogFooter { border-top: 1px solid @border@; }
        QMenuBar { background: @surface@; padding: 4px 8px; }
        QMenuBar::item { padding: 6px 10px; background: transparent; }
        QMenuBar::item:selected, QMenu::item:selected { background: @selected@; color: @selectedText@; border-radius: 4px; }
        QMenu { background: @surface@; border: 1px solid @border@; padding: 6px; }
        QMenu::item { padding: 8px 32px 8px 12px; }
        QMenu::item:disabled { color: @muted@; }
        QMenu::separator { height: 1px; background: @border@; margin: 6px; }
        QToolBar { background: @surface@; border: 0; border-bottom: 1px solid @border@; padding: 6px 12px; spacing: 4px; }
        QToolBar QWidget { background: transparent; }
        QToolBar::separator { background: @border@; width: 1px; margin: 9px 7px; }
        QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 0; }
        QToolButton:hover { background: @hover@; }
        QToolButton:checked { background: @selected@; border-color: @focus@; }
        QToolButton:focus { border-color: @focus@; }
        QToolButton:disabled { color: @muted@; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: @input@; border: 1px solid @controlBorder@; border-radius: 6px;
            padding: 7px 10px; min-height: 18px; selection-background-color: @selected@; selection-color: @selectedText@;
        }
        QLineEdit, QComboBox { placeholder-text-color: @muted@; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QKeySequenceEdit:focus { border-color: @focus@; }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: @muted@; background: @raised@; }
        QComboBox { padding-right: 32px; }
        QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: top right; width: 28px; border: 0; }
        QComboBox::down-arrow { image: url(:/ikview/style/down-@theme@.svg); width: 12px; height: 12px; }
        QComboBox::down-arrow:disabled { image: url(:/ikview/style/down-@theme@-disabled.svg); }
        QComboBox QAbstractItemView { border: 1px solid @border@; background: @surface@; padding: 4px; selection-background-color: @selected@; }
        QSpinBox, QDoubleSpinBox { padding-right: 28px; }
        QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 24px; border-left: 1px solid @border@; border-bottom: 1px solid @border@; border-top-right-radius: 6px; }
        QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 24px; border-left: 1px solid @border@; border-bottom-right-radius: 6px; }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @hover@; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/ikview/style/up-@theme@.svg); width: 10px; height: 8px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/ikview/style/down-@theme@.svg); width: 10px; height: 8px; }
        QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled { image: url(:/ikview/style/up-@theme@-disabled.svg); }
        QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled { image: url(:/ikview/style/down-@theme@-disabled.svg); }
        QCheckBox { spacing: 8px; min-height: 24px; }
        QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid @controlBorder@; border-radius: 4px; background: @input@; }
        QCheckBox::indicator:checked { background: @selected@; border-color: @focus@; image: url(:/ikview/style/check-@theme@.svg); }
        QCheckBox::indicator:checked:disabled { image: url(:/ikview/style/check-@theme@-disabled.svg); }
        QCheckBox::indicator:hover { border-color: @focus@; }
        QCheckBox::indicator:focus { border-color: @focus@; }
        QCheckBox:disabled { color: @muted@; }
        QPushButton { background: @raised@; border: 1px solid @border@; border-radius: 6px; padding: 7px 14px; min-height: 18px; }
        QPushButton:hover { background: @hover@; border-color: @muted@; }
        QPushButton:pressed { background: @selected@; }
        QPushButton:focus { border-color: @focus@; }
        QPushButton:default, QPushButton[primary="true"] { background: @selected@; border-color: @focus@; color: @selectedText@; font-weight: 600; }
        QPushButton:disabled { color: @muted@; background: @surface@; border-color: @border@; }
        QDialogButtonBox QPushButton { min-width: 64px; }
        QPushButton[subtle="true"] { background: transparent; border: 1px solid transparent; color: @muted@; padding: 5px 10px; }
        QPushButton[subtle="true"]:hover { background: @hover@; color: @text@; }
        QPushButton[destructive="true"] { color: @danger@; }
        QPushButton[subtle="true"]:disabled, QPushButton[destructive="true"]:disabled { color: @muted@; background: transparent; border-color: transparent; }
        QListView, QListWidget, QTreeWidget, QTableWidget { background: @surface@; alternate-background-color: @window@; border: 1px solid transparent; outline: 0; }
        QListView:focus, QListWidget:focus, QTreeWidget:focus, QTableWidget:focus { border-color: @focus@; }
        QListView::item, QListWidget::item { padding: 9px 12px; border-bottom: 1px solid @border@; border-left: 3px solid transparent; }
        QListView::item:selected, QListWidget::item:selected { background: @selected@; color: @selectedText@; border-left-color: @accent@; }
        QListView::item:hover:!selected, QListWidget::item:hover:!selected { background: @hover@; }
        QTreeWidget::item, QTableWidget::item { padding: 7px 10px; border: 0; }
        QTreeWidget::item:selected, QTableWidget::item:selected { background: @selected@; color: @selectedText@; }
        QHeaderView::section { background: @raised@; color: @muted@; border: 0; border-bottom: 1px solid @border@; padding: 9px 10px; font-size: 12px; font-weight: 500; }
        QTableCornerButton::section { background: @raised@; border: 0; }
        QKeySequenceEdit { background: transparent; border: 0; padding: 0; }
        QKeySequenceEdit QLineEdit { border: 1px solid @controlBorder@; border-radius: 5px; padding: 4px 8px; min-height: 18px; background: @input@; }
        QTabWidget::pane { border: 0; }
        QTabBar::tab { padding: 11px 24px; color: @muted@; background: transparent; border: 0; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: @selectedText@; border-bottom-color: @accent@; }
        QTabBar::tab:hover:!selected { background: @hover@; }
        QGroupBox { background: @surface@; border: 1px solid @border@; border-radius: 8px; margin-top: 12px; padding: 18px 16px 16px; }
        QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; font-weight: 600; }
        QScrollArea { background: transparent; border: 0; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
        QScrollBar::handle:vertical { background: @border@; border-radius: 3px; min-height: 28px; }
        QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
        QScrollBar::handle:horizontal { background: @border@; border-radius: 3px; min-width: 28px; }
        QScrollBar::handle:hover { background: @muted@; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QProgressBar { background: @raised@; border: 0; border-radius: 3px; min-height: 6px; max-height: 6px; }
        QProgressBar::chunk { background: @accent@; border-radius: 3px; }
        QProgressBar#fileLoadingProgress { min-height: 3px; max-height: 3px; }
        QStatusBar { background: @surface@; border-top: 1px solid @border@; }
        QStatusBar QLabel { background: transparent; color: @muted@; padding: 4px 10px; font-size: 12px; }
        QStatusBar::item { border: 0; }
        QSplitter::handle { background: @border@; }
        QToolTip { color: @text@; background: @surface@; border: 1px solid @border@; padding: 6px 8px; }
        QLabel#appBrand { font-size: 22px; font-weight: 700; color: @accent@; }
        QWidget#filePanel, QWidget#metadataPanel { background: @surface@; }
        QLabel#listTitle, QLabel#infoTitle { font-size: 14px; font-weight: 600; }
        QLabel#fileCount { color: @muted@; padding: 2px 6px; }
        QLabel#emptyFileListLabel, QLabel#listHint { color: @muted@; font-size: 12px; }
        QLabel#emptyFileListLabel { padding: 20px; }
        QLabel#listHint { padding: 12px 16px; }
        QWidget#imageFooter { background: @surface@; border-top: 1px solid @border@; }
        QLabel#imageFilename { color: @text@; }
        QLabel#imagePosition { color: @muted@; font-size: 12px; }
        QPushButton#goToImageButton { background: transparent; border: 1px solid transparent; color: @accent@; padding: 5px 8px; min-height: 18px; }
        QPushButton#goToImageButton:hover { background: @hover@; }
        QPushButton#goToImageButton:disabled { color: @muted@; background: transparent; border-color: transparent; }
        QSpinBox#pdfPageNumber { padding-top: 5px; padding-bottom: 5px; }
        QWidget#errorBanner { background: @dangerSurface@; }
        QWidget#errorBanner QLabel { color: @danger@; }
        QWidget#sourceScopeHint { background: @selected@; }
        QWidget#sourceScopeHint QLabel { color: @selectedText@; }
        QLabel#emptyStateTitle { font-size: 15px; font-weight: 600; }
        QPlainTextEdit { background: @input@; color: @text@; border: 1px solid @border@; border-radius: 6px; padding: 12px; selection-background-color: @selected@; }
    )");
    stylesheet.replace("@theme@", dark ? "dark" : "light");
    for (const auto &token : tokens) stylesheet.replace(token.first, token.second.name());
    qApp->setStyleSheet(stylesheet);
}
}
