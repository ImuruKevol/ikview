#pragma once

#include <QString>
#include <QMessageBox>

class QWidget;
class QDialog;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;

namespace ik {
QWidget *dialogHeading(const QString &title, const QString &description, QWidget *parent = nullptr);
QWidget *dialogFooter(QDialogButtonBox *buttons, const QString &hint = {}, QWidget *parent = nullptr);
void configureDialog(QDialog *dialog, int width, int height);
void configureForm(QFormLayout *form);
void setMessage(QLabel *label, const QString &message, const QString &tone = QStringLiteral("muted"));
void useDetailList(QListWidget *list);
void setDetailText(QListWidgetItem *item, const QString &detail);
int integerPrompt(QWidget *parent, const QString &title, const QString &label, int value, int minimum, int maximum, int step, bool *accepted);
double decimalPrompt(QWidget *parent, const QString &title, const QString &label, double value, double minimum, double maximum, int decimals, bool *accepted);
QString textPrompt(QWidget *parent, const QString &title, const QString &label, const QString &value, bool *accepted);
QMessageBox::StandardButton questionDialog(QWidget *parent, const QString &title, const QString &text,
    QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
    QMessageBox::StandardButton defaultButton = QMessageBox::No, const QString &affirmative = QStringLiteral("계속"));
void warningDialog(QWidget *parent, const QString &title, const QString &text);
void informationDialog(QWidget *parent, const QString &title, const QString &text);
}
