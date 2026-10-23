#include "dialoglayout.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QInputDialog>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>

namespace ik {
namespace {
constexpr int detailRole = Qt::UserRole + 12;
class DetailListDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(160, 64); }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem background(option); initStyleOption(&background, index);
        auto title = background.text; title.replace('\n', QStringLiteral(" ↵ ")); title.replace('\r', QStringLiteral(" ↵ "));
        background.text.clear(); background.icon = QIcon();
        auto *style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        painter->save(); painter->setClipRect(option.rect);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QRect area = option.rect.adjusted(16, 9, -16, -9);
        auto font = option.font; font.setWeight(QFont::DemiBold); painter->setFont(font);
        painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(QRect(area.x(), area.y(), area.width(), 23), Qt::AlignLeading | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(title, Qt::ElideMiddle, area.width()));
        font.setWeight(QFont::Normal); font.setPixelSize(12); painter->setFont(font);
        painter->setPen(QApplication::palette().color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(QRect(area.x(), area.y() + 25, area.width(), 20), Qt::AlignLeading | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(index.data(detailRole).toString().simplified(), Qt::ElideMiddle, area.width()));
        painter->restore();
    }
};
}
QWidget *dialogHeading(const QString &title, const QString &description, QWidget *parent) {
    auto *header = new QWidget(parent); header->setObjectName("dialogHeading");
    auto *layout = new QVBoxLayout(header); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(6);
    auto *heading = new QLabel(title); heading->setObjectName("dialogTitle"); heading->setWordWrap(true); layout->addWidget(heading);
    if (!description.isEmpty()) { auto *note = new QLabel(description); note->setObjectName("dialogDescription"); note->setWordWrap(true); layout->addWidget(note); }
    return header;
}
QWidget *dialogFooter(QDialogButtonBox *buttons, const QString &hint, QWidget *parent) {
    auto *footer = new QWidget(parent); footer->setObjectName("dialogFooter");
    auto *layout = new QHBoxLayout(footer); layout->setContentsMargins(0, 16, 0, 0); layout->setSpacing(16);
    if (!hint.isEmpty()) { auto *note = new QLabel(hint); note->setObjectName("dialogFooterHint"); note->setWordWrap(true); layout->addWidget(note, 1); }
    else layout->addStretch();
    buttons->setCenterButtons(false);
    if (auto *button = buttons->button(QDialogButtonBox::Save)) { button->setText("저장"); button->setDefault(true); button->setProperty("primary", true); }
    if (auto *button = buttons->button(QDialogButtonBox::Cancel)) { button->setText("취소"); button->setAutoDefault(false); }
    if (auto *button = buttons->button(QDialogButtonBox::Close)) button->setText("닫기");
    layout->addWidget(buttons); return footer;
}
void configureDialog(QDialog *dialog, int width, int height) {
    dialog->resize(width, height);
    if (auto *layout = dialog->layout()) { layout->setContentsMargins(24, 24, 24, 20); layout->setSpacing(16); }
}
void configureForm(QFormLayout *form) {
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter); form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(20); form->setVerticalSpacing(12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
}
void setMessage(QLabel *label, const QString &message, const QString &tone) {
    if (label->property("tone").toString() != tone) { label->setProperty("tone", tone); label->style()->unpolish(label); label->style()->polish(label); }
    label->setText(message); label->setVisible(!message.isEmpty());
}
void useDetailList(QListWidget *list) {
    list->setItemDelegate(new DetailListDelegate(list)); list->setUniformItemSizes(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
}
void setDetailText(QListWidgetItem *item, const QString &detail) { item->setData(detailRole, detail); }
namespace {
void configurePrompt(QInputDialog &dialog, const QString &title, const QString &label, const QString &accept) {
    dialog.setWindowTitle(title); dialog.setLabelText(label); dialog.setOkButtonText(accept); dialog.setCancelButtonText("취소");
    configureDialog(&dialog, 420, 180); dialog.setMinimumWidth(360);
}
QMessageBox::StandardButton messageDialog(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text,
                                        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton, const QString &affirmative = {}) {
    QMessageBox box(icon, title, text, buttons, parent); box.setOption(QMessageBox::Option::DontUseNativeDialog); box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(defaultButton); box.setMinimumWidth(380);
    if (auto *label = box.findChild<QLabel *>("qt_msgbox_label")) { label->setWordWrap(true); label->setMaximumWidth(520); label->setTextInteractionFlags(Qt::TextSelectableByMouse); }
    if (auto *button = box.button(QMessageBox::Yes)) button->setText(affirmative);
    if (auto *button = box.button(QMessageBox::No)) button->setText("취소");
    if (auto *button = box.button(QMessageBox::Ok)) button->setText("확인");
    return static_cast<QMessageBox::StandardButton>(box.exec());
}
}
int integerPrompt(QWidget *parent, const QString &title, const QString &label, int value, int minimum, int maximum, int step, bool *accepted) {
    QInputDialog dialog(parent); dialog.setInputMode(QInputDialog::IntInput); dialog.setIntRange(minimum, maximum); dialog.setIntStep(step); dialog.setIntValue(value);
    configurePrompt(dialog, title, label, "이동"); const bool result = dialog.exec() == QDialog::Accepted; if (accepted) *accepted = result; return dialog.intValue();
}
double decimalPrompt(QWidget *parent, const QString &title, const QString &label, double value, double minimum, double maximum, int decimals, bool *accepted) {
    QInputDialog dialog(parent); dialog.setInputMode(QInputDialog::DoubleInput); dialog.setDoubleRange(minimum, maximum); dialog.setDoubleDecimals(decimals); dialog.setDoubleValue(value);
    configurePrompt(dialog, title, label, "적용"); const bool result = dialog.exec() == QDialog::Accepted; if (accepted) *accepted = result; return dialog.doubleValue();
}
QString textPrompt(QWidget *parent, const QString &title, const QString &label, const QString &value, bool *accepted) {
    QInputDialog dialog(parent); dialog.setInputMode(QInputDialog::TextInput); dialog.setTextValue(value);
    configurePrompt(dialog, title, label, "변경"); const bool result = dialog.exec() == QDialog::Accepted; if (accepted) *accepted = result; return dialog.textValue();
}
QMessageBox::StandardButton questionDialog(QWidget *parent, const QString &title, const QString &text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton, const QString &affirmative) {
    return messageDialog(parent, QMessageBox::Question, title, text, buttons, defaultButton, affirmative);
}
void warningDialog(QWidget *parent, const QString &title, const QString &text) { messageDialog(parent, QMessageBox::Warning, title, text, QMessageBox::Ok, QMessageBox::Ok); }
void informationDialog(QWidget *parent, const QString &title, const QString &text) { messageDialog(parent, QMessageBox::Information, title, text, QMessageBox::Ok, QMessageBox::Ok); }
}
