#ifndef UISTYLE_H
#define UISTYLE_H

#include <QString>

// Shared Qt Widgets style for the acquisition device and the console.
// Derived from CollectionSystem/res/css/dark_special.css:
//   primary #1c6ef2, hover #095de2, border #e6e7ef,
//   page background #f5f7fa, input background #f7f8fa,
//   table header #e7e9ed, text #282b36 / #86909c.
//
// The device uses larger metrics so the touch screen remains readable.
namespace UiStyle {

struct Metrics {
    int base;
    int table;
    int header;
    int input;
    int inputHeight;
    int button;
    int buttonHeight;
    int log;
    int title;
};

inline QString makeSheet(const Metrics& m)
{
    QString qss = QStringLiteral(R"QSS(
QWidget {
    background-color: #f5f7fa;
    color: #282b36;
    font-family: "Microsoft YaHei";
    font-size: @BASE@px;
}
QMainWindow, QDialog, QMessageBox {
    background-color: #f5f7fa;
}
QLabel {
    background: transparent;
    color: #282b36;
}
QLabel:disabled {
    color: #86909c;
}
QLabel#syncLabel {
    background-color: #ffffff;
    border: 2px solid #8fa0b8;
    border-radius: 8px;
    padding: 8px 12px;
    font-size: @BASE@px;
    font-weight: bold;
    color: #1c6ef2;
}
QGroupBox {
    background-color: #ffffff;
    border: 2px solid #8fa0b8;
    border-radius: 10px;
    margin-top: 26px;
    padding: 6px 10px 10px 10px;
    font-weight: 600;
    color: #282b36;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 14px;
    top: 0px;
    padding: 2px 10px;
    color: #1c6ef2;
    background-color: #f5f7fa;
    font-size: @TITLE@px;
    font-weight: bold;
}
QLineEdit, QSpinBox, QDoubleSpinBox {
    background-color: #ffffff;
    border: 2px solid #8fa0b8;
    border-radius: 6px;
    padding: 6px 10px;
    min-height: @INPUTH@px;
    color: #282b36;
    font-size: @INPUT@px;
    selection-background-color: #1c6ef2;
    selection-color: #ffffff;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {
    border-color: #1c6ef2;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 2px solid #1c6ef2;
    background-color: #ffffff;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    color: #86909c;
    background-color: #eef0f4;
}
QLineEdit:read-only {
    color: #4e5969;
    background-color: #eef1f6;
}
QPushButton {
    background-color: #1c6ef2;
    color: #ffffff;
    border: none;
    border-radius: 6px;
    padding: 6px 18px;
    min-height: @BUTTONH@px;
    font-size: @BUTTON@px;
    font-weight: 500;
}
QPushButton:hover {
    background-color: #095de2;
}
QPushButton:pressed {
    background-color: #095de2;
}
QPushButton:disabled {
    background-color: #bbd4fc;
    color: #ffffff;
}
QPushButton#secondaryButton {
    background-color: #ffffff;
    color: #1c6ef2;
    border: 1px solid #1c6ef2;
}
QPushButton#secondaryButton:hover {
    background-color: #ecf2fe;
}
QPushButton#dangerButton {
    background-color: #d32424;
}
QPushButton#dangerButton:hover {
    background-color: #b91c1c;
}
QCheckBox {
    background: transparent;
    color: #282b36;
    spacing: 8px;
    font-size: @BASE@px;
}
QTableWidget, QTableView {
    background-color: #ffffff;
    alternate-background-color: #fafbfd;
    border: 2px solid #8fa0b8;
    border-radius: 8px;
    gridline-color: #dfe3ea;
    color: #282b36;
    font-size: @TABLE@px;
    selection-background-color: #ecf2fe;
    selection-color: #1c6ef2;
}
QTableWidget::item, QTableView::item {
    padding: 10px 8px;
    border-bottom: 1px solid #eef0f4;
}
QTableWidget::item:selected, QTableView::item:selected {
    background-color: #ecf2fe;
    color: #1c6ef2;
}
QHeaderView::section {
    background-color: #e7e9ed;
    color: #282b36;
    border: none;
    border-right: 1px solid #d5dae3;
    border-bottom: 2px solid #8fa0b8;
    padding: 10px 8px;
    font-size: @HEADER@px;
    font-weight: bold;
}
QTableCornerButton::section {
    background-color: #e7e9ed;
    border: none;
}
QListWidget {
    background-color: #ffffff;
    border: 2px solid #8fa0b8;
    border-radius: 8px;
    padding: 6px;
    color: #282b36;
    font-size: @BASE@px;
}
QListWidget::item {
    padding: 10px 8px;
    border-radius: 6px;
}
QListWidget::item:hover {
    background-color: #f4f5f9;
}
QListWidget::item:selected {
    background-color: #ecf2fe;
    color: #1c6ef2;
}
QTabWidget::pane {
    border: 2px solid #8fa0b8;
    border-radius: 8px;
    background-color: #ffffff;
    top: -1px;
}
QTabBar::tab {
    background-color: #eef1f6;
    color: #4e5969;
    border: 1px solid #e6e7ef;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 7px 16px;
    margin-right: 4px;
    font-size: @BASE@px;
}
QTabBar::tab:hover {
    color: #1c6ef2;
}
QTabBar::tab:selected {
    background-color: #ffffff;
    color: #1c6ef2;
    border-bottom: 2px solid #1c6ef2;
}
QPlainTextEdit, QTextEdit {
    background-color: #ffffff;
    border: 2px solid #8fa0b8;
    border-radius: 8px;
    padding: 6px;
    color: #282b36;
    font-family: "Consolas";
    font-size: @LOG@px;
}
QScrollBar:vertical {
    width: 8px;
    background: transparent;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background-color: #cfd6e4;
    min-height: 24px;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background-color: #1c6ef2;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: none;
}
QScrollBar:horizontal {
    height: 8px;
    background: transparent;
    margin: 0px;
}
QScrollBar::handle:horizontal {
    background-color: #cfd6e4;
    min-width: 24px;
    border-radius: 4px;
}
QScrollBar::handle:horizontal:hover {
    background-color: #1c6ef2;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0px;
}
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: none;
}
QSplitter::handle {
    background-color: #e6e7ef;
}
QSplitter::handle:horizontal {
    width: 6px;
}
QSplitter::handle:vertical {
    height: 6px;
}
QToolTip {
    background-color: #ffffff;
    color: #282b36;
    border: 1px solid #1c6ef2;
    border-radius: 4px;
    padding: 6px;
}
)QSS");

    qss.replace("@BASE@", QString::number(m.base));
    qss.replace("@TABLE@", QString::number(m.table));
    qss.replace("@HEADER@", QString::number(m.header));
    qss.replace("@INPUT@", QString::number(m.input));
    qss.replace("@INPUTH@", QString::number(m.inputHeight));
    qss.replace("@BUTTON@", QString::number(m.button));
    qss.replace("@BUTTONH@", QString::number(m.buttonHeight));
    qss.replace("@LOG@", QString::number(m.log));
    qss.replace("@TITLE@", QString::number(m.title));
    return qss;
}

// Acquisition device: larger fonts and touch-friendly controls.
inline QString deviceSheet()
{
    return makeSheet(Metrics{16, 16, 15, 16, 40, 16, 42, 14, 18});
}

// Console: denser layout, but same visual language.
inline QString consoleSheet()
{
    return makeSheet(Metrics{13, 14, 14, 14, 32, 14, 34, 12, 14});
}

} // namespace UiStyle

#endif // UISTYLE_H
